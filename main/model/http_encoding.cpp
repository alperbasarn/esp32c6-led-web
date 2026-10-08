#include "http_encoding.h"

#include <stddef.h>

namespace {

bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

// Case-insensitive compare of [begin,end) against a NUL-terminated literal.
bool token_is(const char *begin, const char *end, const char *literal)
{
    for (const char *p = begin; p < end; ++p, ++literal) {
        if (*literal == '\0' || lower(*p) != lower(*literal)) {
            return false;
        }
    }
    return *literal == '\0';
}

// qvalue in thousandths: "1" -> 1000, "0.5" -> 500, "0" -> 0. RFC 9110 allows
// at most three fractional digits; extra digits are ignored rather than
// rejected, since refusing to parse would mean refusing the response.
int parse_qvalue(const char *begin, const char *end)
{
    while (begin < end && is_space(*begin)) {
        ++begin;
    }
    if (begin >= end || (*begin != '0' && *begin != '1')) {
        return 1000;  // malformed -> treat as absent
    }
    int whole = *begin++ - '0';
    int q = whole * 1000;
    if (begin < end && *begin == '.') {
        ++begin;
        int scale = 100;
        while (begin < end && *begin >= '0' && *begin <= '9' && scale > 0) {
            q += (*begin++ - '0') * scale;
            scale /= 10;
        }
    }
    return q > 1000 ? 1000 : q;
}

// The qvalue attached to one Accept-Encoding element, i.e. everything after the
// coding name. Absent or unparsable parameters mean q=1.
int element_qvalue(const char *begin, const char *end)
{
    for (const char *p = begin; p < end; ++p) {
        if (*p != ';') {
            continue;
        }
        const char *param = p + 1;
        while (param < end && is_space(*param)) {
            ++param;
        }
        if (param < end && lower(*param) == 'q') {
            ++param;
            while (param < end && is_space(*param)) {
                ++param;
            }
            if (param < end && *param == '=') {
                return parse_qvalue(param + 1, end);
            }
        }
    }
    return 1000;
}

}  // namespace

bool http_accepts_gzip(const char *accept_encoding)
{
    // No field at all: any coding is acceptable.
    if (accept_encoding == NULL) {
        return true;
    }

    bool saw_gzip = false;
    int gzip_q = 0;
    bool saw_star = false;
    int star_q = 0;

    const char *cursor = accept_encoding;
    while (*cursor != '\0') {
        const char *element = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            ++cursor;
        }
        const char *element_end = cursor;
        if (*cursor == ',') {
            ++cursor;
        }

        // Trim the element, then isolate the coding name from its parameters.
        while (element < element_end && is_space(*element)) {
            ++element;
        }
        while (element_end > element && is_space(*(element_end - 1))) {
            --element_end;
        }
        if (element == element_end) {
            continue;  // empty element, e.g. a stray comma
        }

        const char *name_end = element;
        while (name_end < element_end && *name_end != ';') {
            ++name_end;
        }
        const char *trimmed_name_end = name_end;
        while (trimmed_name_end > element && is_space(*(trimmed_name_end - 1))) {
            --trimmed_name_end;
        }

        if (token_is(element, trimmed_name_end, "gzip") ||
            token_is(element, trimmed_name_end, "x-gzip")) {
            // Several gzip entries are malformed; the most permissive wins, so a
            // later q=0 cannot silently veto an earlier positive one.
            int q = element_qvalue(name_end, element_end);
            if (!saw_gzip || q > gzip_q) {
                gzip_q = q;
            }
            saw_gzip = true;
        } else if (token_is(element, trimmed_name_end, "*")) {
            int q = element_qvalue(name_end, element_end);
            if (!saw_star || q > star_q) {
                star_q = q;
            }
            saw_star = true;
        }
    }

    // An explicit gzip entry is more specific than the wildcard, so it decides
    // even when it forbids and the wildcard allows.
    if (saw_gzip) {
        return gzip_q > 0;
    }
    if (saw_star) {
        return star_q > 0;
    }
    // A value that named codings but not gzip (and no wildcard) leaves identity
    // as the only acceptable coding.
    return false;
}

bool http_accepts_gzip_truncated(void)
{
    return true;
}
