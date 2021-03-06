#include "alpha.h"
#include "scan.h"
#include "labelQueue.h"
#include "scanstack.h"
#include "dec.h"
#include "hex.h"
#include <assert.h>

static inline int shouldIgnore(char ch) {
    return ch != '('
        && ch != ')'
        && ch != ','
        && ch != ':'
        && ch != '/'
        && (ch < '0' || ch > '9')
        && (ch < 'A' || ch > 'Z')
        && (ch < 'a' || ch > 'z')
    ;
}

static uint32_t programCounter;

void scanInit() {
    programCounter = (uint32_t)-1;
    labelQueueInit();
}

int scanValid(const char **iter) {
    return **iter || !scanstackEmpty();
}

int isHexConstantPrefix(const char *iter) {
    return *(uint16_t *)iter == 'x0';
}

int isConstant(const char *iter) {
    return isDecimal(*iter);  
}

op_t parseHex(const char **iter) {
    const char *start = *iter;
    while (isHex(**iter)) ++(*iter);
    uint8_t words = 0;
    const char *end = *iter;
    while (end > start) {
        words++;
        if (end - start >= 2) {
            end -= 2;
            scanstackPush((op_t)hexString16ToUint8(end));
        } else {
            --end;
            scanstackPush((op_t)hexString8ToUint8(*end));
        }
    }
    assert(words <= 32);
    return (op_t)((PUSH1 - 1) + words);
}

static inline uint64_t hi32(uint64_t in) {
    return in >> 32;
}

static inline uint64_t lo32(uint64_t in) {
    return in & 0xffffffff;
}

op_t parseDecimal(const char **iter) {
    uint64_t words[4] = {0,0,0,0};
    while (isDecimal(**iter)) {
        // multiply number by 10
        for (uint8_t i = 4; i --> 0;) {
            // long multiplication
            uint64_t s0, s1, s2, s3;

            uint64_t x = lo32(words[i]) * 10;
            s0 = lo32(x);

            x = hi32(words[i]) * 10 + hi32(x);
            s1 = lo32(x);
            s2 = hi32(x);

            x = s1;
            s1 = lo32(x);

            x = s2 + hi32(x);
            s2 = lo32(x);
            s3 = hi32(x);

            words[i] = s1 << 32 | s0;
            uint64_t carry = s3 << 32 | s2;
            // add carry
            for (uint8_t j = i + 1; j < 4; j++) {
                words[j] += carry;
                if (carry > words[j]) {
                    carry = 1;
                } else break;
            }
        }
        uint8_t digit = *((*iter)++) - '0';
        // add digit
        for (uint8_t i = 0; i < 4; i++) {
            words[i] += digit;
            if (digit > words[i]) {
                digit = 1;
            } else break;
        }
    }
    uint8_t start = 0;
    for (uint8_t i = 4; i --> 0;) {
        for (uint8_t j = 8; j --> 0;) {
            uint64_t shift = j * 8;
            uint8_t word = (words[i] & (0xffllu << shift)) >> shift;
            if (word) {
                start = i * 8 + j;
                i = 0;
                break;
            }
        }
    }
    for (uint8_t i = 0; i < 4; i++) {
        for (uint8_t j = 0; j < 8; j++) {
            if (i * 8 + j > start) {
                break;
            }
            uint64_t shift = j * 8;
            uint8_t word = (words[i] & (0xffllu << shift)) >> shift;
            scanstackPush(word);
        }
    }
    return (op_t) PUSH1 + start;
}

op_t parseConstant(const char **iter) {
    if (isHexConstantPrefix(*iter)) {
        (*iter) += 2;
        return parseHex(iter);
    } else {
        return parseDecimal(iter);
    }
}

// For FUNCTION(ARG1,ARG2) the op order is ARG2 ARG1 FUNCTION
// For FN1(FN11(ARG11,ARG12), FN12(ARG21,ARG22)) the op order is ARG22 ARG21 FN12 ARG12 ARG11 FN11 FN1


void scanChar(const char **iter, char expected) {
    for (char ch; (ch = **iter) != expected; (*iter)++) {
        if (!shouldIgnore(ch)) {
            fprintf(stderr, "When seeking %c found unexpected character %c, before: %s", expected, ch, *iter);
            assert(expected == ch);
        }
    }
    (*iter)++;
}

static void scanComment(const char **iter) {
    for (char ch; (ch = **iter) != '\n'; (*iter)++);
    (*iter)++;
}

static inline char scanWaste(const char **iter) {
    char ch;
    for (; shouldIgnore(ch = **iter) && ch; (*iter)++);
    if (ch == '/') {
        scanComment(iter);
        return scanWaste(iter);
    }
    return ch;
}

static void scanLabel(const char **iter) {
    const char *start = *iter;
    for (char ch; isLowerCase(**iter); (*iter)++);
    const char *end = *iter;
    char next = scanWaste(iter);
    if (next == ':') {
        (*iter)++;
        scanstackPushLabel(start, end - start, JUMPDEST);
    } else {
        scanstackPushLabel(start, end - start, STOP);
        scanstackPush(PUSH1);
    }
}

static void scanOp(const char **iter) {
    scanWaste(iter);
    if (isConstant(*iter)) {
        op_t op = parseConstant(iter);
        scanWaste(iter);
        scanstackPush(op);
        return;
    } else if (isLowerCase(**iter)) {
        scanLabel(iter);
        return;
    }
    op_t op = parseOp(*iter, iter);
    char next = scanWaste(iter);
    scanstackPush(op);
    if (next != '(') {
        return;
    }
    scanChar(iter, '(');
    for (uint8_t i = 0; i < argCount[op]; i++) {
        if (i) {
            scanWaste(iter);
            scanChar(iter, ',');
        }
        scanWaste(iter);
        if (isConstant(*iter)) {
            scanstackPush(parseConstant(iter));
        } else if (isLowerCase(**iter)) {
            scanLabel(iter);
        } else {
            const char *end;
            op_t next = parseOp(*iter, &end);
            // TODO maybe next can be read from stack after scanOp instead of parsing twice
            assert(retCount[next] != 0);
            i += (retCount[next] - 1);
            scanOp(iter);
        }
    }
    scanWaste(iter);
    scanChar(iter, ')');
    scanWaste(iter);
}

op_t scanNextOp(const char **iter) {
    jump_t jump;
    programCounter++;
    jump.programCounter = programCounter;
    if (!scanstackEmpty()) {
        if (scanstackTopLabel(&jump.label)) {
            op_t type = scanstackPop();
            if (type == JUMPDEST) {
                registerLabel(jump);
            } else {
                labelQueuePush(jump);
            }
            return type;
        } else return scanstackPop();
    }
    scanOp(iter);
    if (scanstackTopLabel(&jump.label)) {
        op_t type = scanstackPop();
        if (type == JUMPDEST) {
            registerLabel(jump);
        } else {
            labelQueuePush(jump);
        }
        return type;
    } else return scanstackPop();
}

void scanFinalize(op_t *begin, uint32_t *programLength) {
    // TODO handle labels for programs longer than 256
    while (!labelQueueEmpty()) {
        jump_t jump = labelQueuePop();
        uint32_t location = getLabelLocation(jump.label);
        if (location >= 256) {
            fprintf(stderr, "Unsupported label location %u\n", location);
        }
        begin[jump.programCounter] = location;
    }
}
