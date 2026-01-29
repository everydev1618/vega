#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// Helper Functions
// ============================================================================

static void emit_byte(CodeGen* cg, uint8_t byte) {
    if (cg->code_size >= cg->code_capacity) {
        cg->code_capacity = cg->code_capacity == 0 ? 256 : cg->code_capacity * 2;
        cg->code = realloc(cg->code, cg->code_capacity);
    }
    cg->code[cg->code_size++] = byte;
}

static void emit_u16(CodeGen* cg, uint16_t value) {
    emit_byte(cg, value & 0xFF);
    emit_byte(cg, (value >> 8) & 0xFF);
}

static void emit_i16(CodeGen* cg, int16_t value) {
    emit_u16(cg, (uint16_t)value);
}

static void emit_u32(CodeGen* cg, uint32_t value) {
    emit_byte(cg, value & 0xFF);
    emit_byte(cg, (value >> 8) & 0xFF);
    emit_byte(cg, (value >> 16) & 0xFF);
    emit_byte(cg, (value >> 24) & 0xFF);
}

static uint32_t current_offset(CodeGen* cg) {
    return cg->code_size;
}

static void patch_jump(CodeGen* cg, uint32_t offset, int16_t jump) {
    cg->code[offset] = jump & 0xFF;
    cg->code[offset + 1] = (jump >> 8) & 0xFF;
}

// Process escape sequences in a string
static char* process_escapes(const char* str, uint32_t length, uint32_t* out_length) {
    char* result = malloc(length + 1);
    uint32_t j = 0;

    for (uint32_t i = 0; i < length; i++) {
        if (str[i] == '\\' && i + 1 < length) {
            i++;
            switch (str[i]) {
                case 'n':  result[j++] = '\n'; break;
                case 'r':  result[j++] = '\r'; break;
                case 't':  result[j++] = '\t'; break;
                case '\\': result[j++] = '\\'; break;
                case '"':  result[j++] = '"'; break;
                case '0':  result[j++] = '\0'; break;
                default:   result[j++] = str[i]; break;
            }
        } else {
            result[j++] = str[i];
        }
    }
    result[j] = '\0';
    *out_length = j;
    return result;
}

// Add string to constant pool, return index
static uint16_t add_string_constant(CodeGen* cg, const char* str, uint32_t length) {
    // Process escape sequences
    uint32_t processed_len;
    char* processed = process_escapes(str, length, &processed_len);

    // Check if already interned
    for (uint32_t i = 0; i < cg->string_count; i++) {
        if (strlen(cg->strings[i]) == processed_len &&
            memcmp(cg->strings[i], processed, processed_len) == 0) {
            free(processed);
            return cg->string_indices[i];
        }
    }

    // Add to constant pool
    uint16_t idx = cg->const_size;

    // Grow if needed
    if (cg->const_size + 3 + processed_len >= cg->const_capacity) {
        cg->const_capacity = cg->const_capacity == 0 ? 1024 : cg->const_capacity * 2;
        cg->constants = realloc(cg->constants, cg->const_capacity);
    }

    // Write string constant: type (1) + length (2) + data
    cg->constants[cg->const_size++] = CONST_STRING;
    cg->constants[cg->const_size++] = processed_len & 0xFF;
    cg->constants[cg->const_size++] = (processed_len >> 8) & 0xFF;
    memcpy(cg->constants + cg->const_size, processed, processed_len);
    cg->const_size += processed_len;

    // Intern for deduplication
    if (cg->string_count >= cg->string_capacity) {
        cg->string_capacity = cg->string_capacity == 0 ? 64 : cg->string_capacity * 2;
        cg->strings = realloc(cg->strings, cg->string_capacity * sizeof(char*));
        cg->string_indices = realloc(cg->string_indices, cg->string_capacity * sizeof(uint16_t));
    }
    cg->strings[cg->string_count] = processed;  // transfer ownership
    cg->string_indices[cg->string_count] = idx;
    cg->string_count++;

    return idx;
}

static uint16_t add_int_constant(CodeGen* cg, int64_t value) {
    uint16_t idx = cg->const_size;

    if (cg->const_size + 5 >= cg->const_capacity) {
        cg->const_capacity = cg->const_capacity == 0 ? 1024 : cg->const_capacity * 2;
        cg->constants = realloc(cg->constants, cg->const_capacity);
    }

    cg->constants[cg->const_size++] = CONST_INT;
    int32_t val32 = (int32_t)value;
    memcpy(cg->constants + cg->const_size, &val32, 4);
    cg->const_size += 4;

    return idx;
}

// Local variable management
static int find_local(CodeGen* cg, const char* name) {
    for (uint32_t i = 0; i < cg->local_count; i++) {
        if (strcmp(cg->locals[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static uint8_t add_local(CodeGen* cg, const char* name) {
    int existing = find_local(cg, name);
    if (existing >= 0) return (uint8_t)existing;

    if (cg->local_count >= cg->local_capacity) {
        cg->local_capacity = cg->local_capacity == 0 ? 16 : cg->local_capacity * 2;
        cg->locals = realloc(cg->locals, cg->local_capacity * sizeof(char*));
    }

    uint8_t slot = (uint8_t)cg->local_count;
    cg->locals[cg->local_count++] = strdup(name);
    return slot;
}

static void clear_locals(CodeGen* cg) {
    for (uint32_t i = 0; i < cg->local_count; i++) {
        free(cg->locals[i]);
    }
    cg->local_count = 0;
}

// Loop tracking for break/continue
static void push_loop(CodeGen* cg, uint32_t loop_start) {
    if (cg->loop_depth >= cg->loop_capacity) {
        cg->loop_capacity *= 2;
        cg->loop_starts = realloc(cg->loop_starts, cg->loop_capacity * sizeof(uint32_t));
        cg->break_patches = realloc(cg->break_patches, cg->loop_capacity * 8 * sizeof(uint32_t));
    }
    cg->loop_starts[cg->loop_depth] = loop_start;
    cg->loop_depth++;
}

static void pop_loop(CodeGen* cg) {
    if (cg->loop_depth > 0) {
        cg->loop_depth--;
    }
}

static void add_break_patch(CodeGen* cg, uint32_t offset) {
    // Store break patch offset for current loop level
    cg->break_patches[cg->break_count++] = offset;
}

static void patch_breaks(CodeGen* cg, uint32_t loop_end, uint32_t break_start_count) {
    // Patch all breaks since break_start_count
    for (uint32_t i = break_start_count; i < cg->break_count; i++) {
        uint32_t offset = cg->break_patches[i];
        patch_jump(cg, offset, (int16_t)(loop_end - offset - 2));
    }
    cg->break_count = break_start_count;
}

// ============================================================================
// Expression Code Generation
// ============================================================================

static void emit_expr(CodeGen* cg, AstExpr* expr);

static void emit_binary(CodeGen* cg, AstExpr* expr) {
    emit_expr(cg, expr->as.binary.left);
    emit_expr(cg, expr->as.binary.right);

    switch (expr->as.binary.op) {
        case BINOP_ADD: emit_byte(cg, OP_ADD); break;
        case BINOP_SUB: emit_byte(cg, OP_SUB); break;
        case BINOP_MUL: emit_byte(cg, OP_MUL); break;
        case BINOP_DIV: emit_byte(cg, OP_DIV); break;
        case BINOP_MOD: emit_byte(cg, OP_MOD); break;
        case BINOP_EQ:  emit_byte(cg, OP_EQ); break;
        case BINOP_NE:  emit_byte(cg, OP_NE); break;
        case BINOP_LT:  emit_byte(cg, OP_LT); break;
        case BINOP_LE:  emit_byte(cg, OP_LE); break;
        case BINOP_GT:  emit_byte(cg, OP_GT); break;
        case BINOP_GE:  emit_byte(cg, OP_GE); break;
        case BINOP_AND: emit_byte(cg, OP_AND); break;
        case BINOP_OR:  emit_byte(cg, OP_OR); break;
    }
}

static void emit_expr(CodeGen* cg, AstExpr* expr) {
    if (!expr) return;

    switch (expr->kind) {
        case EXPR_INT_LITERAL:
            if (expr->as.int_val >= -128 && expr->as.int_val <= 127) {
                emit_byte(cg, OP_PUSH_INT);
                emit_u32(cg, (uint32_t)(int32_t)expr->as.int_val);
            } else {
                uint16_t idx = add_int_constant(cg, expr->as.int_val);
                emit_byte(cg, OP_PUSH_CONST);
                emit_u16(cg, idx);
            }
            break;

        case EXPR_FLOAT_LITERAL: {
            // Store as constant
            uint16_t idx = cg->const_size;
            if (cg->const_size + 9 >= cg->const_capacity) {
                cg->const_capacity *= 2;
                cg->constants = realloc(cg->constants, cg->const_capacity);
            }
            cg->constants[cg->const_size++] = CONST_FLOAT;
            memcpy(cg->constants + cg->const_size, &expr->as.float_val, 8);
            cg->const_size += 8;
            emit_byte(cg, OP_PUSH_CONST);
            emit_u16(cg, idx);
            break;
        }

        case EXPR_STRING_LITERAL: {
            uint16_t idx = add_string_constant(cg, expr->as.string_val.value,
                                               expr->as.string_val.length);
            emit_byte(cg, OP_PUSH_CONST);
            emit_u16(cg, idx);
            break;
        }

        case EXPR_BOOL_LITERAL:
            emit_byte(cg, expr->as.bool_val ? OP_PUSH_TRUE : OP_PUSH_FALSE);
            break;

        case EXPR_NULL_LITERAL:
            emit_byte(cg, OP_PUSH_NULL);
            break;

        case EXPR_IDENTIFIER: {
            int slot = find_local(cg, expr->as.ident.name);
            if (slot >= 0) {
                emit_byte(cg, OP_LOAD_LOCAL);
                emit_byte(cg, (uint8_t)slot);
            } else {
                // Global lookup - use constant for name
                uint16_t idx = add_string_constant(cg, expr->as.ident.name,
                                                   strlen(expr->as.ident.name));
                emit_byte(cg, OP_LOAD_GLOBAL);
                emit_u16(cg, idx);
            }
            break;
        }

        case EXPR_BINARY:
            emit_binary(cg, expr);
            break;

        case EXPR_UNARY:
            emit_expr(cg, expr->as.unary.operand);
            if (expr->as.unary.op == UNOP_NEG) {
                emit_byte(cg, OP_NEG);
            } else {
                emit_byte(cg, OP_NOT);
            }
            break;

        case EXPR_CALL: {
            // Push arguments in order
            for (uint32_t i = 0; i < expr->as.call.arg_count; i++) {
                emit_expr(cg, expr->as.call.args[i]);
            }

            // Check for built-ins and stdlib
            if (expr->as.call.callee->kind == EXPR_IDENTIFIER) {
                const char* name = expr->as.call.callee->as.ident.name;

                if (strcmp(name, "print") == 0) {
                    emit_byte(cg, OP_PRINT);
                    return;
                }

                // Check for module::function
                if (strstr(name, "::")) {
                    uint16_t idx = add_string_constant(cg, name, strlen(name));
                    emit_byte(cg, OP_CALL_NATIVE);
                    emit_u16(cg, idx);
                    return;
                }
            }

            // Regular function call
            emit_expr(cg, expr->as.call.callee);
            emit_byte(cg, OP_CALL);
            emit_byte(cg, (uint8_t)expr->as.call.arg_count);
            break;
        }

        case EXPR_METHOD_CALL: {
            emit_expr(cg, expr->as.method_call.object);
            for (uint32_t i = 0; i < expr->as.method_call.arg_count; i++) {
                emit_expr(cg, expr->as.method_call.args[i]);
            }
            uint16_t idx = add_string_constant(cg, expr->as.method_call.method,
                                               strlen(expr->as.method_call.method));
            emit_byte(cg, OP_CALL_METHOD);
            emit_u16(cg, idx);
            emit_byte(cg, (uint8_t)expr->as.method_call.arg_count);
            break;
        }

        case EXPR_FIELD_ACCESS: {
            emit_expr(cg, expr->as.field_access.object);
            uint16_t idx = add_string_constant(cg, expr->as.field_access.field,
                                               strlen(expr->as.field_access.field));
            emit_byte(cg, OP_GET_FIELD);
            emit_u16(cg, idx);
            break;
        }

        case EXPR_SPAWN: {
            uint16_t idx = add_string_constant(cg, expr->as.spawn.agent_name,
                                               strlen(expr->as.spawn.agent_name));
            if (expr->as.spawn.is_supervised && expr->as.spawn.supervision) {
                // Supervised spawn: emit OP_SPAWN_SUPERVISED with config
                emit_byte(cg, OP_SPAWN_SUPERVISED);
                emit_u16(cg, idx);
                // Emit supervision config: strategy (1), max_restarts (4), window_ms (4)
                emit_byte(cg, (uint8_t)expr->as.spawn.supervision->strategy);
                emit_u32(cg, expr->as.spawn.supervision->max_restarts);
                emit_u32(cg, expr->as.spawn.supervision->window_ms);
            } else {
                emit_byte(cg, expr->as.spawn.is_async ? OP_SPAWN_ASYNC : OP_SPAWN_AGENT);
                emit_u16(cg, idx);
            }
            break;
        }

        case EXPR_MESSAGE:
            emit_expr(cg, expr->as.message.target);
            emit_expr(cg, expr->as.message.message);
            if (expr->as.message.is_async) {
                emit_byte(cg, OP_SEND_ASYNC);  // Returns future
            } else {
                emit_byte(cg, OP_SEND_MSG);    // Blocks until response
            }
            break;

        case EXPR_AWAIT:
            emit_expr(cg, expr->as.await.future);
            emit_byte(cg, OP_AWAIT);
            break;

        case EXPR_ARRAY_LITERAL: {
            // Create new array with initial capacity
            emit_byte(cg, OP_ARRAY_NEW);
            emit_u16(cg, (uint16_t)expr->as.array_literal.count);
            // Push each element
            for (uint32_t i = 0; i < expr->as.array_literal.count; i++) {
                emit_expr(cg, expr->as.array_literal.elements[i]);
                emit_byte(cg, OP_ARRAY_PUSH);
            }
            break;
        }

        case EXPR_INDEX:
            emit_expr(cg, expr->as.index.object);
            emit_expr(cg, expr->as.index.index);
            emit_byte(cg, OP_ARRAY_GET);
            break;

        case EXPR_OK:
            emit_expr(cg, expr->as.result_val.value);
            emit_byte(cg, OP_RESULT_OK);
            break;

        case EXPR_ERR:
            emit_expr(cg, expr->as.result_val.value);
            emit_byte(cg, OP_RESULT_ERR);
            break;

        case EXPR_MATCH: {
            // Emit scrutinee (the Result value)
            emit_expr(cg, expr->as.match.scrutinee);

            // For each arm, emit: DUP, IS_OK check, jump if not match, unwrap, body
            // Simple approach: check is_ok, branch accordingly

            // We only handle the simple case of Ok(x) => ... and Err(x) => ...
            emit_byte(cg, OP_DUP);  // Keep copy for second arm
            emit_byte(cg, OP_RESULT_IS_OK);

            // Jump to err arm if not ok
            emit_byte(cg, OP_JUMP_IF_NOT);
            uint32_t to_err = current_offset(cg);
            emit_u16(cg, 0);  // Placeholder

            // Ok arm: unwrap and execute body
            // Find the Ok arm
            for (uint32_t i = 0; i < expr->as.match.arm_count; i++) {
                if (expr->as.match.arms[i].is_ok) {
                    // Bind the unwrapped value
                    uint8_t slot = add_local(cg, expr->as.match.arms[i].binding_name);
                    emit_byte(cg, OP_RESULT_UNWRAP);
                    emit_byte(cg, OP_STORE_LOCAL);
                    emit_byte(cg, slot);
                    emit_expr(cg, expr->as.match.arms[i].body);
                    break;
                }
            }

            // Jump over err arm
            emit_byte(cg, OP_JUMP);
            uint32_t to_end = current_offset(cg);
            emit_u16(cg, 0);

            // Patch jump to err arm
            int16_t err_offset = (int16_t)(current_offset(cg) - to_err - 2);
            patch_jump(cg, to_err, err_offset);

            // Err arm: unwrap error and execute body
            for (uint32_t i = 0; i < expr->as.match.arm_count; i++) {
                if (!expr->as.match.arms[i].is_ok) {
                    uint8_t slot = add_local(cg, expr->as.match.arms[i].binding_name);
                    emit_byte(cg, OP_RESULT_UNWRAP);
                    emit_byte(cg, OP_STORE_LOCAL);
                    emit_byte(cg, slot);
                    emit_expr(cg, expr->as.match.arms[i].body);
                    break;
                }
            }

            // Patch end jump
            int16_t end_offset = (int16_t)(current_offset(cg) - to_end - 2);
            patch_jump(cg, to_end, end_offset);
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// Statement Code Generation
// ============================================================================

static void emit_stmt(CodeGen* cg, AstStmt* stmt);

static void emit_block(CodeGen* cg, AstStmt* block) {
    if (!block || block->kind != STMT_BLOCK) return;

    for (uint32_t i = 0; i < block->as.block.stmt_count; i++) {
        emit_stmt(cg, block->as.block.stmts[i]);
    }
}

static void emit_stmt(CodeGen* cg, AstStmt* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case STMT_EXPR:
            emit_expr(cg, stmt->as.expr.expr);
            emit_byte(cg, OP_POP);  // Discard result
            break;

        case STMT_LET: {
            uint8_t slot = add_local(cg, stmt->as.let.name);
            if (stmt->as.let.init) {
                emit_expr(cg, stmt->as.let.init);
            } else {
                emit_byte(cg, OP_PUSH_NULL);
            }
            emit_byte(cg, OP_STORE_LOCAL);
            emit_byte(cg, slot);
            break;
        }

        case STMT_ASSIGN: {
            if (stmt->as.assign.target->kind == EXPR_IDENTIFIER) {
                emit_expr(cg, stmt->as.assign.value);
                int slot = find_local(cg, stmt->as.assign.target->as.ident.name);
                if (slot >= 0) {
                    emit_byte(cg, OP_STORE_LOCAL);
                    emit_byte(cg, (uint8_t)slot);
                } else {
                    uint16_t idx = add_string_constant(cg,
                        stmt->as.assign.target->as.ident.name,
                        strlen(stmt->as.assign.target->as.ident.name));
                    emit_byte(cg, OP_STORE_GLOBAL);
                    emit_u16(cg, idx);
                }
            } else if (stmt->as.assign.target->kind == EXPR_INDEX) {
                // Array index assignment: arr[idx] = value
                emit_expr(cg, stmt->as.assign.target->as.index.object);
                emit_expr(cg, stmt->as.assign.target->as.index.index);
                emit_expr(cg, stmt->as.assign.value);
                emit_byte(cg, OP_ARRAY_SET);
            }
            break;
        }

        case STMT_IF: {
            emit_expr(cg, stmt->as.if_stmt.condition);

            emit_byte(cg, OP_JUMP_IF_NOT);
            uint32_t then_jump = current_offset(cg);
            emit_i16(cg, 0);  // Placeholder

            emit_block(cg, stmt->as.if_stmt.then_branch);

            if (stmt->as.if_stmt.else_branch) {
                emit_byte(cg, OP_JUMP);
                uint32_t else_jump = current_offset(cg);
                emit_i16(cg, 0);

                patch_jump(cg, then_jump, (int16_t)(current_offset(cg) - then_jump - 2));

                if (stmt->as.if_stmt.else_branch->kind == STMT_BLOCK) {
                    emit_block(cg, stmt->as.if_stmt.else_branch);
                } else {
                    emit_stmt(cg, stmt->as.if_stmt.else_branch);
                }

                patch_jump(cg, else_jump, (int16_t)(current_offset(cg) - else_jump - 2));
            } else {
                patch_jump(cg, then_jump, (int16_t)(current_offset(cg) - then_jump - 2));
            }
            break;
        }

        case STMT_WHILE: {
            uint32_t loop_start = current_offset(cg);
            uint32_t break_start = cg->break_count;
            push_loop(cg, loop_start);

            emit_expr(cg, stmt->as.while_stmt.condition);
            emit_byte(cg, OP_JUMP_IF_NOT);
            uint32_t exit_jump = current_offset(cg);
            emit_i16(cg, 0);

            emit_block(cg, stmt->as.while_stmt.body);

            emit_byte(cg, OP_JUMP);
            emit_i16(cg, (int16_t)(loop_start - current_offset(cg) - 2));

            patch_jump(cg, exit_jump, (int16_t)(current_offset(cg) - exit_jump - 2));
            patch_breaks(cg, current_offset(cg), break_start);
            pop_loop(cg);
            break;
        }

        case STMT_FOR: {
            // for (init; condition; update) { body }
            // Emit: init; loop_start: condition; jump_if_not exit; body; update; jump loop_start; exit:

            // Init statement
            if (stmt->as.for_stmt.init) {
                emit_stmt(cg, stmt->as.for_stmt.init);
            }

            uint32_t loop_start = current_offset(cg);
            uint32_t break_start = cg->break_count;
            push_loop(cg, loop_start);

            // Condition
            uint32_t exit_jump = 0;
            if (stmt->as.for_stmt.condition) {
                emit_expr(cg, stmt->as.for_stmt.condition);
                emit_byte(cg, OP_JUMP_IF_NOT);
                exit_jump = current_offset(cg);
                emit_i16(cg, 0);
            }

            // Body
            emit_block(cg, stmt->as.for_stmt.body);

            // Update - this is where continue jumps to
            uint32_t update_start = current_offset(cg);
            if (stmt->as.for_stmt.update) {
                emit_expr(cg, stmt->as.for_stmt.update);
                emit_byte(cg, OP_POP);  // Discard update result
            }

            // Jump back to condition
            emit_byte(cg, OP_JUMP);
            emit_i16(cg, (int16_t)(loop_start - current_offset(cg) - 2));

            // Patch exit jump
            if (stmt->as.for_stmt.condition) {
                patch_jump(cg, exit_jump, (int16_t)(current_offset(cg) - exit_jump - 2));
            }

            // Patch breaks
            patch_breaks(cg, current_offset(cg), break_start);

            // Patch continues to jump to update (stored in loop_starts as update location)
            // Note: For simplicity, continue jumps to loop_start (condition), not update
            // A more complete impl would track update_start separately

            pop_loop(cg);
            (void)update_start;  // Suppress unused warning for now
            break;
        }

        case STMT_BREAK: {
            if (cg->loop_depth == 0) {
                snprintf(cg->error_msg, sizeof(cg->error_msg),
                        "break outside of loop");
                cg->had_error = true;
                break;
            }
            emit_byte(cg, OP_JUMP);
            add_break_patch(cg, current_offset(cg));
            emit_i16(cg, 0);  // Placeholder, will be patched
            break;
        }

        case STMT_CONTINUE: {
            if (cg->loop_depth == 0) {
                snprintf(cg->error_msg, sizeof(cg->error_msg),
                        "continue outside of loop");
                cg->had_error = true;
                break;
            }
            // Jump back to loop start (condition check)
            uint32_t loop_start = cg->loop_starts[cg->loop_depth - 1];
            emit_byte(cg, OP_JUMP);
            emit_i16(cg, (int16_t)(loop_start - current_offset(cg) - 2));
            break;
        }

        case STMT_RETURN:
            if (stmt->as.return_stmt.value) {
                emit_expr(cg, stmt->as.return_stmt.value);
            } else {
                emit_byte(cg, OP_PUSH_NULL);
            }
            emit_byte(cg, OP_RETURN);
            break;

        case STMT_BLOCK:
            emit_block(cg, stmt);
            break;

        default:
            break;
    }
}

// ============================================================================
// Declaration Code Generation
// ============================================================================

static void emit_function(CodeGen* cg, FunctionDecl* fn) {
    clear_locals(cg);

    // Add parameters as locals
    for (uint32_t i = 0; i < fn->param_count; i++) {
        add_local(cg, fn->params[i].name);
    }

    // Record function start
    uint32_t start_offset = current_offset(cg);

    // Emit body
    if (fn->body) {
        for (uint32_t i = 0; i < fn->body->as.block.stmt_count; i++) {
            emit_stmt(cg, fn->body->as.block.stmts[i]);
        }
    }

    // Add implicit return if needed
    if (cg->code_size == start_offset || cg->code[cg->code_size - 1] != OP_RETURN) {
        emit_byte(cg, OP_PUSH_NULL);
        emit_byte(cg, OP_RETURN);
    }

    // Record function in table
    if (cg->func_count >= cg->func_capacity) {
        cg->func_capacity = cg->func_capacity == 0 ? 16 : cg->func_capacity * 2;
        cg->functions = realloc(cg->functions, cg->func_capacity * sizeof(FunctionDef));
    }

    FunctionDef* func = &cg->functions[cg->func_count++];
    func->name_idx = add_string_constant(cg, fn->name, strlen(fn->name));
    func->param_count = (uint16_t)fn->param_count;
    func->local_count = (uint16_t)cg->local_count;
    func->code_offset = start_offset;
    func->code_length = current_offset(cg) - start_offset;
}

static void emit_tool(CodeGen* cg, const char* agent_name, ToolDecl* tool) {
    // Emit tool as a function with name "AgentName$toolname"
    clear_locals(cg);

    // Add parameters as locals
    for (uint32_t i = 0; i < tool->param_count; i++) {
        add_local(cg, tool->params[i].name);
    }

    // Record function start
    uint32_t start_offset = current_offset(cg);

    // Emit body
    if (tool->body) {
        for (uint32_t i = 0; i < tool->body->as.block.stmt_count; i++) {
            emit_stmt(cg, tool->body->as.block.stmts[i]);
        }
    }

    // Add implicit return if needed
    if (cg->code_size == start_offset || cg->code[cg->code_size - 1] != OP_RETURN) {
        emit_byte(cg, OP_PUSH_NULL);
        emit_byte(cg, OP_RETURN);
    }

    // Build qualified name: "AgentName$toolname"
    char qualified_name[256];
    snprintf(qualified_name, sizeof(qualified_name), "%s$%s", agent_name, tool->name);

    // Record function in table
    if (cg->func_count >= cg->func_capacity) {
        cg->func_capacity = cg->func_capacity == 0 ? 16 : cg->func_capacity * 2;
        cg->functions = realloc(cg->functions, cg->func_capacity * sizeof(FunctionDef));
    }

    FunctionDef* func = &cg->functions[cg->func_count++];
    func->name_idx = add_string_constant(cg, qualified_name, strlen(qualified_name));
    func->param_count = (uint16_t)tool->param_count;
    func->local_count = (uint16_t)cg->local_count;
    func->code_offset = start_offset;
    func->code_length = current_offset(cg) - start_offset;

    // Store parameter names and types as a constant string: "name1:type1,name2:type2"
    if (tool->param_count > 0) {
        char params_str[1024] = "";
        int offset = 0;
        for (uint32_t i = 0; i < tool->param_count; i++) {
            if (i > 0) params_str[offset++] = ',';
            offset += snprintf(params_str + offset, sizeof(params_str) - offset,
                "%s:%s",
                tool->params[i].name,
                tool->params[i].type.name ? tool->params[i].type.name : "str");
        }
        // Store with qualified name + "$params" suffix
        char params_key[280];
        snprintf(params_key, sizeof(params_key), "%s$params", qualified_name);
        add_string_constant(cg, params_key, strlen(params_key));
        add_string_constant(cg, params_str, strlen(params_str));
    }
}

static void emit_agent(CodeGen* cg, AgentDecl* agent) {
    // First, emit all tools as functions
    for (uint32_t i = 0; i < agent->tool_count; i++) {
        emit_tool(cg, agent->name, &agent->tools[i]);
    }

    if (cg->agent_count >= cg->agent_capacity) {
        cg->agent_capacity = cg->agent_capacity == 0 ? 8 : cg->agent_capacity * 2;
        cg->agents = realloc(cg->agents, cg->agent_capacity * sizeof(AgentDef));
    }

    AgentDef* def = &cg->agents[cg->agent_count++];
    def->name_idx = add_string_constant(cg, agent->name, strlen(agent->name));
    def->model_idx = agent->model ?
        add_string_constant(cg, agent->model, strlen(agent->model)) : 0;
    def->system_idx = agent->system_prompt ?
        add_string_constant(cg, agent->system_prompt, strlen(agent->system_prompt)) : 0;
    def->tool_count = (uint16_t)agent->tool_count;
    def->temperature_x100 = (uint16_t)(agent->temperature * 100);
}

// ============================================================================
// Public API
// ============================================================================

void codegen_init(CodeGen* cg) {
    memset(cg, 0, sizeof(CodeGen));
    cg->const_capacity = 1024;
    cg->constants = malloc(cg->const_capacity);
    cg->loop_capacity = 16;
    cg->loop_starts = malloc(cg->loop_capacity * sizeof(uint32_t));
    cg->break_patches = malloc(cg->loop_capacity * 8 * sizeof(uint32_t)); // Allow 8 breaks per loop level
}

void codegen_cleanup(CodeGen* cg) {
    free(cg->code);
    free(cg->constants);
    for (uint32_t i = 0; i < cg->string_count; i++) {
        free(cg->strings[i]);
    }
    free(cg->strings);
    free(cg->string_indices);
    free(cg->functions);
    free(cg->agents);
    for (uint32_t i = 0; i < cg->local_count; i++) {
        free(cg->locals[i]);
    }
    free(cg->locals);
    free(cg->loop_starts);
    free(cg->break_patches);
}

bool codegen_generate(CodeGen* cg, AstProgram* program) {
    // First pass: emit agents
    for (uint32_t i = 0; i < program->decl_count; i++) {
        if (program->decls[i]->kind == DECL_AGENT) {
            emit_agent(cg, &program->decls[i]->as.agent);
        }
    }

    // Second pass: emit functions
    for (uint32_t i = 0; i < program->decl_count; i++) {
        if (program->decls[i]->kind == DECL_FUNCTION) {
            emit_function(cg, &program->decls[i]->as.function);
        }
    }

    return !cg->had_error;
}

bool codegen_write_file(CodeGen* cg, const char* filename) {
    FILE* f = fopen(filename, "wb");
    if (!f) {
        snprintf(cg->error_msg, sizeof(cg->error_msg),
                "Cannot open output file: %s", filename);
        cg->had_error = true;
        return false;
    }

    // Write header
    VegaHeader header = {
        .magic = VEGA_MAGIC,
        .version = VEGA_VERSION,
        .flags = 0,
        .const_pool_size = cg->const_size,
        .code_size = cg->code_size
    };
    fwrite(&header, sizeof(header), 1, f);

    // Write function count and agent count
    uint16_t func_count = (uint16_t)cg->func_count;
    uint16_t agent_count = (uint16_t)cg->agent_count;
    fwrite(&func_count, sizeof(func_count), 1, f);
    fwrite(&agent_count, sizeof(agent_count), 1, f);

    // Write function table
    fwrite(cg->functions, sizeof(FunctionDef), cg->func_count, f);

    // Write agent definitions
    fwrite(cg->agents, sizeof(AgentDef), cg->agent_count, f);

    // Write constant pool
    fwrite(cg->constants, 1, cg->const_size, f);

    // Write code
    fwrite(cg->code, 1, cg->code_size, f);

    fclose(f);
    return true;
}

void codegen_disassemble(CodeGen* cg, FILE* out) {
    fprintf(out, "; Vega Bytecode Disassembly\n");
    fprintf(out, "; Constants: %u bytes, Code: %u bytes\n\n",
            cg->const_size, cg->code_size);

    // Print functions
    fprintf(out, "; Functions: %u\n", cg->func_count);
    for (uint32_t i = 0; i < cg->func_count; i++) {
        FunctionDef* fn = &cg->functions[i];
        fprintf(out, ";   [%u] offset=%u len=%u params=%u locals=%u\n",
                i, fn->code_offset, fn->code_length, fn->param_count, fn->local_count);
    }
    fprintf(out, "\n");

    // Print agents
    fprintf(out, "; Agents: %u\n", cg->agent_count);
    for (uint32_t i = 0; i < cg->agent_count; i++) {
        AgentDef* ag = &cg->agents[i];
        fprintf(out, ";   [%u] name_idx=%u model_idx=%u tools=%u temp=%u\n",
                i, ag->name_idx, ag->model_idx, ag->tool_count, ag->temperature_x100);
    }
    fprintf(out, "\n");

    // Disassemble code
    fprintf(out, "; Code:\n");
    uint32_t ip = 0;
    while (ip < cg->code_size) {
        fprintf(out, "%04x: ", ip);

        uint8_t op = cg->code[ip++];

        switch (op) {
            case OP_NOP:          fprintf(out, "NOP\n"); break;
            case OP_PUSH_CONST:   fprintf(out, "PUSH_CONST %u\n", READ_U16(cg->code, ip)); ip += 2; break;
            case OP_PUSH_INT:     fprintf(out, "PUSH_INT %d\n", (int32_t)READ_U32(cg->code, ip)); ip += 4; break;
            case OP_PUSH_TRUE:    fprintf(out, "PUSH_TRUE\n"); break;
            case OP_PUSH_FALSE:   fprintf(out, "PUSH_FALSE\n"); break;
            case OP_PUSH_NULL:    fprintf(out, "PUSH_NULL\n"); break;
            case OP_POP:          fprintf(out, "POP\n"); break;
            case OP_DUP:          fprintf(out, "DUP\n"); break;
            case OP_LOAD_LOCAL:   fprintf(out, "LOAD_LOCAL %u\n", cg->code[ip++]); break;
            case OP_STORE_LOCAL:  fprintf(out, "STORE_LOCAL %u\n", cg->code[ip++]); break;
            case OP_LOAD_GLOBAL:  fprintf(out, "LOAD_GLOBAL %u\n", READ_U16(cg->code, ip)); ip += 2; break;
            case OP_STORE_GLOBAL: fprintf(out, "STORE_GLOBAL %u\n", READ_U16(cg->code, ip)); ip += 2; break;
            case OP_ADD:          fprintf(out, "ADD\n"); break;
            case OP_SUB:          fprintf(out, "SUB\n"); break;
            case OP_MUL:          fprintf(out, "MUL\n"); break;
            case OP_DIV:          fprintf(out, "DIV\n"); break;
            case OP_MOD:          fprintf(out, "MOD\n"); break;
            case OP_NEG:          fprintf(out, "NEG\n"); break;
            case OP_EQ:           fprintf(out, "EQ\n"); break;
            case OP_NE:           fprintf(out, "NE\n"); break;
            case OP_LT:           fprintf(out, "LT\n"); break;
            case OP_LE:           fprintf(out, "LE\n"); break;
            case OP_GT:           fprintf(out, "GT\n"); break;
            case OP_GE:           fprintf(out, "GE\n"); break;
            case OP_NOT:          fprintf(out, "NOT\n"); break;
            case OP_AND:          fprintf(out, "AND\n"); break;
            case OP_OR:           fprintf(out, "OR\n"); break;
            case OP_JUMP:         fprintf(out, "JUMP %d\n", READ_I16(cg->code, ip)); ip += 2; break;
            case OP_JUMP_IF:      fprintf(out, "JUMP_IF %d\n", READ_I16(cg->code, ip)); ip += 2; break;
            case OP_JUMP_IF_NOT:  fprintf(out, "JUMP_IF_NOT %d\n", READ_I16(cg->code, ip)); ip += 2; break;
            case OP_CALL:         fprintf(out, "CALL %u\n", cg->code[ip++]); break;
            case OP_RETURN:       fprintf(out, "RETURN\n"); break;
            case OP_CALL_NATIVE:  fprintf(out, "CALL_NATIVE %u\n", READ_U16(cg->code, ip)); ip += 2; break;
            case OP_SPAWN_AGENT:  fprintf(out, "SPAWN_AGENT %u\n", READ_U16(cg->code, ip)); ip += 2; break;
            case OP_SEND_MSG:     fprintf(out, "SEND_MSG\n"); break;
            case OP_SEND_ASYNC:   fprintf(out, "SEND_ASYNC\n"); break;
            case OP_SPAWN_ASYNC:  fprintf(out, "SPAWN_ASYNC %u\n", READ_U16(cg->code, ip)); ip += 2; break;
            case OP_AWAIT:        fprintf(out, "AWAIT\n"); break;
            case OP_GET_FIELD:    fprintf(out, "GET_FIELD %u\n", READ_U16(cg->code, ip)); ip += 2; break;
            case OP_CALL_METHOD:  fprintf(out, "CALL_METHOD %u %u\n", READ_U16(cg->code, ip), cg->code[ip+2]); ip += 3; break;
            case OP_PRINT:        fprintf(out, "PRINT\n"); break;
            case OP_HALT:         fprintf(out, "HALT\n"); break;
            default:              fprintf(out, "UNKNOWN(%02x)\n", op); break;
        }
    }
}

bool codegen_had_error(CodeGen* cg) {
    return cg->had_error;
}

const char* codegen_error_msg(CodeGen* cg) {
    return cg->error_msg;
}
