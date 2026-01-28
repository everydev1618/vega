#!/bin/bash
# Vega Language Completeness Test Suite v0.1
# Runs 20 tests to validate language completeness

set -o pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
VEGAC="$ROOT_DIR/bin/vegac"
VEGA="$ROOT_DIR/bin/vega"

# Create build directory
mkdir -p "$BUILD_DIR"

# Counters
PASSED=0
FAILED=0
SKIPPED=0

# Test names
declare -a TEST_NAMES=(
    "Hello World"
    "FizzBuzz"
    "Fibonacci"
    "Factorial"
    "String Reversal"
    "Array Operations"
    "File Read/Write"
    "JSON Parse"
    "HTTP GET Request"
    "Error Handling"
    "Spawn Single Agent"
    "Agent with Tool"
    "Two Agents Sequence"
    "Parallel Agents"
    "Agent Conversation Loop"
    "Prime Sieve"
    "Binary Search"
    "Merge Sort"
    "Word Frequency"
    "Simple Calculator"
)

# Helper function to print test result
print_result() {
    local test_num=$1
    local test_name=$2
    local status=$3
    local message=$4

    printf "Test %02d: %-25s " "$test_num" "$test_name"

    if [ "$status" = "PASS" ]; then
        echo -e "${GREEN}PASS${NC}"
        ((PASSED++))
    elif [ "$status" = "FAIL" ]; then
        echo -e "${RED}FAIL${NC}"
        if [ -n "$message" ]; then
            echo "         $message"
        fi
        ((FAILED++))
    else
        echo -e "${YELLOW}SKIP${NC}"
        if [ -n "$message" ]; then
            echo "         $message"
        fi
        ((SKIPPED++))
    fi
}

# Helper function to compile a test
compile_test() {
    local test_file=$1
    local output_file=$2

    "$VEGAC" "$test_file" -o "$output_file" 2>&1
    return $?
}

# Helper function to run a test (strips warning lines from output)
run_test() {
    local bytecode_file=$1
    local timeout_sec=${2:-30}

    timeout "$timeout_sec" "$VEGA" "$bytecode_file" 2>&1 | grep -v "^Warning:"
    return ${PIPESTATUS[0]}
}

# Helper function to check line content
check_line() {
    local output=$1
    local line_num=$2
    local expected=$3

    local actual=$(echo "$output" | sed -n "${line_num}p")
    if [ "$actual" = "$expected" ]; then
        return 0
    else
        return 1
    fi
}

# Helper function to count lines
count_lines() {
    local output=$1
    echo "$output" | wc -l | tr -d ' '
}

# Helper function to check if output contains string
contains() {
    local output=$1
    local needle=$2

    if echo "$output" | grep -q "$needle"; then
        return 0
    else
        return 1
    fi
}

echo "========================================"
echo "  Vega Language Completeness Test Suite"
echo "========================================"
echo ""

# Check if binaries exist
if [ ! -x "$VEGAC" ] || [ ! -x "$VEGA" ]; then
    echo -e "${RED}Error: vegac or vega binary not found.${NC}"
    echo "Run 'make' in the root directory first."
    exit 1
fi

echo "Using:"
echo "  vegac: $VEGAC"
echo "  vega:  $VEGA"
echo ""
echo "Running tests..."
echo ""

# =============================================================================
# Test 01: Hello World
# =============================================================================
test_01() {
    local test_file="$SCRIPT_DIR/test_01_hello_world.vega"
    local bytecode="$BUILD_DIR/test_01.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 1 "Hello World" "FAIL" "Compilation failed: $compile_out"
        return
    fi

    local output=$(run_test "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 1 "Hello World" "FAIL" "Runtime error: $output"
        return
    fi

    if [ "$output" = "Hello, Vega!" ]; then
        print_result 1 "Hello World" "PASS"
    else
        print_result 1 "Hello World" "FAIL" "Expected 'Hello, Vega!', got '$output'"
    fi
}

# =============================================================================
# Test 02: FizzBuzz
# =============================================================================
test_02() {
    local test_file="$SCRIPT_DIR/test_02_fizzbuzz.vega"
    local bytecode="$BUILD_DIR/test_02.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 2 "FizzBuzz" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 2 "FizzBuzz" "FAIL" "Runtime error"
        return
    fi

    local line_count=$(count_lines "$output")
    local line3=$(echo "$output" | sed -n '3p')
    local line5=$(echo "$output" | sed -n '5p')
    local line15=$(echo "$output" | sed -n '15p')
    local line7=$(echo "$output" | sed -n '7p')

    if [ "$line_count" != "100" ]; then
        print_result 2 "FizzBuzz" "FAIL" "Expected 100 lines, got $line_count"
    elif [ "$line3" != "Fizz" ]; then
        print_result 2 "FizzBuzz" "FAIL" "Line 3: expected 'Fizz', got '$line3'"
    elif [ "$line5" != "Buzz" ]; then
        print_result 2 "FizzBuzz" "FAIL" "Line 5: expected 'Buzz', got '$line5'"
    elif [ "$line15" != "FizzBuzz" ]; then
        print_result 2 "FizzBuzz" "FAIL" "Line 15: expected 'FizzBuzz', got '$line15'"
    elif [ "$line7" != "7" ]; then
        print_result 2 "FizzBuzz" "FAIL" "Line 7: expected '7', got '$line7'"
    else
        print_result 2 "FizzBuzz" "PASS"
    fi
}

# =============================================================================
# Test 03: Fibonacci
# =============================================================================
test_03() {
    local test_file="$SCRIPT_DIR/test_03_fibonacci.vega"
    local bytecode="$BUILD_DIR/test_03.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 3 "Fibonacci" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode" 60)  # May be slow due to recursion
    if [ $? -ne 0 ]; then
        print_result 3 "Fibonacci" "FAIL" "Runtime error or timeout"
        return
    fi

    local line_count=$(count_lines "$output")
    local line1=$(echo "$output" | sed -n '1p')
    local line2=$(echo "$output" | sed -n '2p')
    local line10=$(echo "$output" | sed -n '10p')
    local line20=$(echo "$output" | sed -n '20p')

    if [ "$line_count" != "20" ]; then
        print_result 3 "Fibonacci" "FAIL" "Expected 20 lines, got $line_count"
    elif [ "$line1" != "0" ]; then
        print_result 3 "Fibonacci" "FAIL" "Line 1: expected '0', got '$line1'"
    elif [ "$line2" != "1" ]; then
        print_result 3 "Fibonacci" "FAIL" "Line 2: expected '1', got '$line2'"
    elif [ "$line10" != "34" ]; then
        print_result 3 "Fibonacci" "FAIL" "Line 10: expected '34', got '$line10'"
    elif [ "$line20" != "4181" ]; then
        print_result 3 "Fibonacci" "FAIL" "Line 20: expected '4181', got '$line20'"
    else
        print_result 3 "Fibonacci" "PASS"
    fi
}

# =============================================================================
# Test 04: Factorial
# =============================================================================
test_04() {
    local test_file="$SCRIPT_DIR/test_04_factorial.vega"
    local bytecode="$BUILD_DIR/test_04.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 4 "Factorial" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 4 "Factorial" "FAIL" "Runtime error"
        return
    fi

    local line_count=$(count_lines "$output")
    local line1=$(echo "$output" | sed -n '1p')
    local line2=$(echo "$output" | sed -n '2p')
    local line6=$(echo "$output" | sed -n '6p')
    local line11=$(echo "$output" | sed -n '11p')

    if [ "$line_count" != "11" ]; then
        print_result 4 "Factorial" "FAIL" "Expected 11 lines, got $line_count"
    elif [ "$line1" != "1" ]; then
        print_result 4 "Factorial" "FAIL" "Line 1: expected '1', got '$line1'"
    elif [ "$line2" != "1" ]; then
        print_result 4 "Factorial" "FAIL" "Line 2: expected '1', got '$line2'"
    elif [ "$line6" != "120" ]; then
        print_result 4 "Factorial" "FAIL" "Line 6: expected '120', got '$line6'"
    elif [ "$line11" != "3628800" ]; then
        print_result 4 "Factorial" "FAIL" "Line 11: expected '3628800', got '$line11'"
    else
        print_result 4 "Factorial" "PASS"
    fi
}

# =============================================================================
# Test 05: String Reversal
# =============================================================================
test_05() {
    local test_file="$SCRIPT_DIR/test_05_string_reversal.vega"
    local bytecode="$BUILD_DIR/test_05.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 5 "String Reversal" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 5 "String Reversal" "FAIL" "Runtime error"
        return
    fi

    if [ "$output" = "rallets si ageV" ]; then
        print_result 5 "String Reversal" "PASS"
    else
        print_result 5 "String Reversal" "FAIL" "Expected 'rallets si ageV', got '$output'"
    fi
}

# =============================================================================
# Test 06: Array Operations
# =============================================================================
test_06() {
    local test_file="$SCRIPT_DIR/test_06_array_operations.vega"
    local bytecode="$BUILD_DIR/test_06.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 6 "Array Operations" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 6 "Array Operations" "FAIL" "Runtime error"
        return
    fi

    local line1=$(echo "$output" | sed -n '1p')
    local line2=$(echo "$output" | sed -n '2p')
    local line3=$(echo "$output" | sed -n '3p')

    if [ "$line1" != "55" ]; then
        print_result 6 "Array Operations" "FAIL" "Line 1: expected '55', got '$line1'"
    elif [ "$line2" != "30" ]; then
        print_result 6 "Array Operations" "FAIL" "Line 2: expected '30', got '$line2'"
    elif [ "$line3" != "[2, 4, 6, 8, 10, 12, 14, 16, 18, 20]" ]; then
        print_result 6 "Array Operations" "FAIL" "Line 3: expected '[2, 4, 6, 8, 10, 12, 14, 16, 18, 20]', got '$line3'"
    else
        print_result 6 "Array Operations" "PASS"
    fi
}

# =============================================================================
# Test 07: File Read/Write
# =============================================================================
test_07() {
    local test_file="$SCRIPT_DIR/test_07_file_io.vega"
    local bytecode="$BUILD_DIR/test_07.vgb"
    local output_file="$BUILD_DIR/test_output.txt"

    # Clean up any previous output file
    rm -f "$output_file"

    # Change to build dir for relative file path
    pushd "$BUILD_DIR" > /dev/null

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        popd > /dev/null
        print_result 7 "File Read/Write" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode")
    local run_status=$?
    popd > /dev/null

    if [ $run_status -ne 0 ]; then
        print_result 7 "File Read/Write" "FAIL" "Runtime error"
        return
    fi

    # Check file exists
    if [ ! -f "$output_file" ]; then
        print_result 7 "File Read/Write" "FAIL" "Output file not created"
        return
    fi

    local line1=$(echo "$output" | sed -n '1p')
    local line2=$(echo "$output" | sed -n '2p')

    if [ "$line1" != "3" ]; then
        print_result 7 "File Read/Write" "FAIL" "Line count: expected '3', got '$line1'"
    elif [ "$line2" != "27" ]; then
        print_result 7 "File Read/Write" "FAIL" "Char count: expected '27', got '$line2'"
    else
        print_result 7 "File Read/Write" "PASS"
    fi

    # Cleanup
    rm -f "$output_file"
}

# =============================================================================
# Test 08: JSON Parse
# =============================================================================
test_08() {
    local test_file="$SCRIPT_DIR/test_08_json_parse.vega"
    local bytecode="$BUILD_DIR/test_08.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 8 "JSON Parse" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 8 "JSON Parse" "FAIL" "Runtime error"
        return
    fi

    local line1=$(echo "$output" | sed -n '1p')
    local line2=$(echo "$output" | sed -n '2p')
    local line3=$(echo "$output" | sed -n '3p')
    local line4=$(echo "$output" | sed -n '4p')

    if [ "$line1" != "Vega" ]; then
        print_result 8 "JSON Parse" "FAIL" "Line 1: expected 'Vega', got '$line1'"
    elif [ "$line2" != "0.1" ]; then
        print_result 8 "JSON Parse" "FAIL" "Line 2: expected '0.1', got '$line2'"
    elif [ "$line3" != "3" ]; then
        print_result 8 "JSON Parse" "FAIL" "Line 3: expected '3', got '$line3'"
    elif [ "$line4" != "tools" ]; then
        print_result 8 "JSON Parse" "FAIL" "Line 4: expected 'tools', got '$line4'"
    else
        print_result 8 "JSON Parse" "PASS"
    fi
}

# =============================================================================
# Test 09: HTTP GET Request
# =============================================================================
test_09() {
    local test_file="$SCRIPT_DIR/test_09_http_get.vega"
    local bytecode="$BUILD_DIR/test_09.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 9 "HTTP GET Request" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode" 60)
    if [ $? -ne 0 ]; then
        print_result 9 "HTTP GET Request" "FAIL" "Runtime error or timeout"
        return
    fi

    if contains "$output" "vega"; then
        print_result 9 "HTTP GET Request" "PASS"
    else
        print_result 9 "HTTP GET Request" "FAIL" "Output doesn't contain 'vega'"
    fi
}

# =============================================================================
# Test 10: Error Handling
# =============================================================================
test_10() {
    local test_file="$SCRIPT_DIR/test_10_error_handling.vega"
    local bytecode="$BUILD_DIR/test_10.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 10 "Error Handling" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 10 "Error Handling" "FAIL" "Runtime error"
        return
    fi

    local line1=$(echo "$output" | sed -n '1p')
    local line2=$(echo "$output" | sed -n '2p')

    if [ "$line1" != "5" ]; then
        print_result 10 "Error Handling" "FAIL" "Line 1: expected '5', got '$line1'"
    elif [ "$line2" != "division by zero" ]; then
        print_result 10 "Error Handling" "FAIL" "Line 2: expected 'division by zero', got '$line2'"
    else
        print_result 10 "Error Handling" "PASS"
    fi
}

# =============================================================================
# Test 11: Spawn Single Agent
# =============================================================================
test_11() {
    local test_file="$SCRIPT_DIR/test_11_spawn_agent.vega"
    local bytecode="$BUILD_DIR/test_11.vgb"

    # Check for API key
    if [ -z "$ANTHROPIC_API_KEY" ]; then
        print_result 11 "Spawn Single Agent" "SKIP" "ANTHROPIC_API_KEY not set"
        return
    fi

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 11 "Spawn Single Agent" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode" 120)
    if [ $? -ne 0 ]; then
        print_result 11 "Spawn Single Agent" "FAIL" "Runtime error or timeout"
        return
    fi

    # Check that output is non-empty (agent responded)
    # Strip any trailing whitespace/newlines for length check
    local trimmed=$(echo "$output" | tr -d '\n' | head -c 200)
    if [ -n "$trimmed" ] && [ ${#trimmed} -gt 0 ]; then
        print_result 11 "Spawn Single Agent" "PASS"
    else
        print_result 11 "Spawn Single Agent" "FAIL" "Response empty"
    fi
}

# =============================================================================
# Test 12: Agent with Tool
# =============================================================================
test_12() {
    local test_file="$SCRIPT_DIR/test_12_agent_tool.vega"
    local bytecode="$BUILD_DIR/test_12.vgb"

    if [ -z "$ANTHROPIC_API_KEY" ]; then
        print_result 12 "Agent with Tool" "SKIP" "ANTHROPIC_API_KEY not set"
        return
    fi

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 12 "Agent with Tool" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode" 120)
    if [ $? -ne 0 ]; then
        print_result 12 "Agent with Tool" "FAIL" "Runtime error or timeout"
        return
    fi

    if contains "$output" "42"; then
        print_result 12 "Agent with Tool" "PASS"
    else
        print_result 12 "Agent with Tool" "FAIL" "Response doesn't contain '42'"
    fi
}

# =============================================================================
# Test 13: Two Agents Sequence
# =============================================================================
test_13() {
    local test_file="$SCRIPT_DIR/test_13_two_agents.vega"
    local bytecode="$BUILD_DIR/test_13.vgb"

    if [ -z "$ANTHROPIC_API_KEY" ]; then
        print_result 13 "Two Agents Sequence" "SKIP" "ANTHROPIC_API_KEY not set"
        return
    fi

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 13 "Two Agents Sequence" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode" 180)
    if [ $? -ne 0 ]; then
        print_result 13 "Two Agents Sequence" "FAIL" "Runtime error or timeout"
        return
    fi

    # Check output is non-empty (both agents responded)
    local word_count=$(echo "$output" | wc -w | tr -d ' ')
    if [ "$word_count" -ge 1 ]; then
        print_result 13 "Two Agents Sequence" "PASS"
    else
        print_result 13 "Two Agents Sequence" "FAIL" "No output from agents"
    fi
}

# =============================================================================
# Test 14: Parallel Agents
# =============================================================================
test_14() {
    local test_file="$SCRIPT_DIR/test_14_parallel_agents.vega"
    local bytecode="$BUILD_DIR/test_14.vgb"

    if [ -z "$ANTHROPIC_API_KEY" ]; then
        print_result 14 "Parallel Agents" "SKIP" "ANTHROPIC_API_KEY not set"
        return
    fi

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 14 "Parallel Agents" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode" 180)
    if [ $? -ne 0 ]; then
        print_result 14 "Parallel Agents" "FAIL" "Runtime error or timeout"
        return
    fi

    local has_alpha=$(contains "$output" "alpha" && echo "yes" || echo "no")
    local has_beta=$(contains "$output" "beta" && echo "yes" || echo "no")
    local has_gamma=$(contains "$output" "gamma" && echo "yes" || echo "no")

    if [ "$has_alpha" = "yes" ] && [ "$has_beta" = "yes" ] && [ "$has_gamma" = "yes" ]; then
        print_result 14 "Parallel Agents" "PASS"
    else
        print_result 14 "Parallel Agents" "FAIL" "Missing alpha/beta/gamma in output"
    fi
}

# =============================================================================
# Test 15: Agent Conversation Loop
# =============================================================================
test_15() {
    local test_file="$SCRIPT_DIR/test_15_agent_loop.vega"
    local bytecode="$BUILD_DIR/test_15.vgb"

    if [ -z "$ANTHROPIC_API_KEY" ]; then
        print_result 15 "Agent Conversation Loop" "SKIP" "ANTHROPIC_API_KEY not set"
        return
    fi

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 15 "Agent Conversation Loop" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode" 300)
    if [ $? -ne 0 ]; then
        print_result 15 "Agent Conversation Loop" "FAIL" "Runtime error or timeout"
        return
    fi

    # Check that output contains numbers 1-5
    local has_1=$(contains "$output" "1" && echo "yes" || echo "no")
    local has_5=$(contains "$output" "5" && echo "yes" || echo "no")

    if [ "$has_1" = "yes" ] && [ "$has_5" = "yes" ]; then
        print_result 15 "Agent Conversation Loop" "PASS"
    else
        print_result 15 "Agent Conversation Loop" "FAIL" "Output doesn't contain expected numbers"
    fi
}

# =============================================================================
# Test 16: Prime Sieve
# =============================================================================
test_16() {
    local test_file="$SCRIPT_DIR/test_16_prime_sieve.vega"
    local bytecode="$BUILD_DIR/test_16.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 16 "Prime Sieve" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 16 "Prime Sieve" "FAIL" "Runtime error"
        return
    fi

    local line_count=$(count_lines "$output")
    local first=$(echo "$output" | sed -n '1p')
    local last=$(echo "$output" | sed -n '25p')

    # Check for some key primes
    local has_17=$(contains "$output" "17" && echo "yes" || echo "no")
    local has_53=$(contains "$output" "53" && echo "yes" || echo "no")
    local has_89=$(contains "$output" "89" && echo "yes" || echo "no")

    if [ "$line_count" != "25" ]; then
        print_result 16 "Prime Sieve" "FAIL" "Expected 25 lines, got $line_count"
    elif [ "$first" != "2" ]; then
        print_result 16 "Prime Sieve" "FAIL" "First prime: expected '2', got '$first'"
    elif [ "$last" != "97" ]; then
        print_result 16 "Prime Sieve" "FAIL" "Last prime: expected '97', got '$last'"
    elif [ "$has_17" != "yes" ] || [ "$has_53" != "yes" ] || [ "$has_89" != "yes" ]; then
        print_result 16 "Prime Sieve" "FAIL" "Missing some expected primes"
    else
        print_result 16 "Prime Sieve" "PASS"
    fi
}

# =============================================================================
# Test 17: Binary Search
# =============================================================================
test_17() {
    local test_file="$SCRIPT_DIR/test_17_binary_search.vega"
    local bytecode="$BUILD_DIR/test_17.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 17 "Binary Search" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 17 "Binary Search" "FAIL" "Runtime error"
        return
    fi

    local line1=$(echo "$output" | sed -n '1p')
    local line2=$(echo "$output" | sed -n '2p')
    local line3=$(echo "$output" | sed -n '3p')
    local line4=$(echo "$output" | sed -n '4p')

    if [ "$line1" != "3" ]; then
        print_result 17 "Binary Search" "FAIL" "Line 1: expected '3', got '$line1'"
    elif [ "$line2" != "0" ]; then
        print_result 17 "Binary Search" "FAIL" "Line 2: expected '0', got '$line2'"
    elif [ "$line3" != "9" ]; then
        print_result 17 "Binary Search" "FAIL" "Line 3: expected '9', got '$line3'"
    elif [ "$line4" != "-1" ]; then
        print_result 17 "Binary Search" "FAIL" "Line 4: expected '-1', got '$line4'"
    else
        print_result 17 "Binary Search" "PASS"
    fi
}

# =============================================================================
# Test 18: Merge Sort
# =============================================================================
test_18() {
    local test_file="$SCRIPT_DIR/test_18_merge_sort.vega"
    local bytecode="$BUILD_DIR/test_18.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 18 "Merge Sort" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 18 "Merge Sort" "FAIL" "Runtime error"
        return
    fi

    if [ "$output" = "[11, 12, 22, 25, 34, 42, 64, 90]" ]; then
        print_result 18 "Merge Sort" "PASS"
    else
        print_result 18 "Merge Sort" "FAIL" "Expected '[11, 12, 22, 25, 34, 42, 64, 90]', got '$output'"
    fi
}

# =============================================================================
# Test 19: Word Frequency
# =============================================================================
test_19() {
    local test_file="$SCRIPT_DIR/test_19_word_frequency.vega"
    local bytecode="$BUILD_DIR/test_19.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 19 "Word Frequency" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 19 "Word Frequency" "FAIL" "Runtime error"
        return
    fi

    # Check for key frequency counts
    local has_the_3=$(contains "$output" "the: 3" && echo "yes" || echo "no")
    local has_fox_2=$(contains "$output" "fox: 2" && echo "yes" || echo "no")
    local has_quick_2=$(contains "$output" "quick: 2" && echo "yes" || echo "no")
    local has_dog_1=$(contains "$output" "dog: 1" && echo "yes" || echo "no")

    if [ "$has_the_3" = "yes" ] && [ "$has_fox_2" = "yes" ] && [ "$has_quick_2" = "yes" ] && [ "$has_dog_1" = "yes" ]; then
        print_result 19 "Word Frequency" "PASS"
    else
        print_result 19 "Word Frequency" "FAIL" "Word counts incorrect"
    fi
}

# =============================================================================
# Test 20: Simple Calculator
# =============================================================================
test_20() {
    local test_file="$SCRIPT_DIR/test_20_calculator.vega"
    local bytecode="$BUILD_DIR/test_20.vgb"

    local compile_out=$(compile_test "$test_file" "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 20 "Simple Calculator" "FAIL" "Compilation failed"
        return
    fi

    local output=$(run_test "$bytecode")
    if [ $? -ne 0 ]; then
        print_result 20 "Simple Calculator" "FAIL" "Runtime error"
        return
    fi

    local line_count=$(count_lines "$output")
    local line1=$(echo "$output" | sed -n '1p')
    local line2=$(echo "$output" | sed -n '2p')
    local line3=$(echo "$output" | sed -n '3p')
    local line4=$(echo "$output" | sed -n '4p')

    if [ "$line_count" != "4" ]; then
        print_result 20 "Simple Calculator" "FAIL" "Expected 4 lines, got $line_count"
    elif [ "$line1" != "5" ]; then
        print_result 20 "Simple Calculator" "FAIL" "Line 1: expected '5', got '$line1'"
    elif [ "$line2" != "6" ]; then
        print_result 20 "Simple Calculator" "FAIL" "Line 2: expected '6', got '$line2'"
    elif [ "$line3" != "42" ]; then
        print_result 20 "Simple Calculator" "FAIL" "Line 3: expected '42', got '$line3'"
    elif [ "$line4" != "5" ]; then
        print_result 20 "Simple Calculator" "FAIL" "Line 4: expected '5', got '$line4'"
    else
        print_result 20 "Simple Calculator" "PASS"
    fi
}

# =============================================================================
# Run all tests
# =============================================================================

test_01
test_02
test_03
test_04
test_05
test_06
test_07
test_08
test_09
test_10
test_11
test_12
test_13
test_14
test_15
test_16
test_17
test_18
test_19
test_20

# =============================================================================
# Summary
# =============================================================================

echo ""
echo "========================================"
echo "                 SUMMARY"
echo "========================================"
TOTAL=$((PASSED + FAILED + SKIPPED))
echo ""
echo "Passed:  $PASSED"
echo "Failed:  $FAILED"
echo "Skipped: $SKIPPED"
echo "Total:   $TOTAL"
echo ""

# Scoring
SCORE=$((PASSED + SKIPPED))
if [ $SCORE -eq 20 ]; then
    echo -e "Status: ${GREEN}Vega is complete${NC}"
elif [ $SCORE -ge 15 ]; then
    echo -e "Status: ${GREEN}Vega is usable${NC}"
elif [ $SCORE -ge 10 ]; then
    echo -e "Status: ${YELLOW}Vega is in progress${NC}"
elif [ $SCORE -ge 1 ]; then
    echo -e "Status: ${YELLOW}Vega is early alpha${NC}"
else
    echo -e "Status: ${RED}Vega doesn't run${NC}"
fi

echo ""
echo "Results: $PASSED/$TOTAL passed"
echo ""

# Exit with number of failures
exit $FAILED
