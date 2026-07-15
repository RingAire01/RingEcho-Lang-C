#include "span.h"

Re0Pos re0_pos_make(size_t line, size_t col, size_t offset) {
    Re0Pos p = { line, col, offset };
    return p;
}

Re0Span re0_span_make(Re0Pos start, Re0Pos end) {
    Re0Span s = { start, end };
    return s;
}
