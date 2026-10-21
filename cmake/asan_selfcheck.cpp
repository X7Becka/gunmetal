// Negative gate for the ASan preset: this program must be killed by ASan.
// The `volatile` index prevents the compiler from folding the out-of-bounds
// access away.  CTest marks the test WILL_FAIL, so a clean exit is a failure.
#include <cstddef>

int main() {
    volatile std::size_t index = 4;
    int* values = new int[2];
    values[index] = 42;
    delete[] values;
    return 0;
}
