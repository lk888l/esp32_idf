#pragma once
#define ESP_RETURN_ON_ERROR(expression, tag, ...) do { \
    (void)(tag); const auto test_result = (expression); \
    if (test_result != ESP_OK) return test_result; \
} while (0)
#define ESP_RETURN_ON_FALSE(condition, code, tag, ...) do { \
    (void)(tag); if (!(condition)) return (code); \
} while (0)
