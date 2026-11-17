#include "pch.h"
#include "json.h"


#define check(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "Check failed: " << #expr << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::abort(); \
        } \
    } while (0)

ARCH_EXPORT void TestExtractJsonFields()
{
    enum class TestEnum {
        None = 0,
        Value = 7,
    };

    {
        simdjson::ondemand::parser parser;
        auto json = simdjson::padded_string(std::string_view(R"({"object":{"inner":11}})"));
        simdjson::ondemand::document doc;
        check(parser.iterate(json).get(doc) == simdjson::SUCCESS);

        JsonObject root;
        check(doc.get_object().get(root) == simdjson::SUCCESS);

        int innerValue = 0;
        check(ExtractJsonFields(root,
            Field("object", [&](JsonObject objectValue) {
                check(ExtractJsonFields(objectValue, Field("inner", innerValue)) == 1);
            })) == 1);
        check(innerValue == 11);
    }

    {
        simdjson::ondemand::parser parser;
        auto json = simdjson::padded_string(std::string_view(R"({"array":[1,2,3]})"));
        simdjson::ondemand::document doc;
        check(parser.iterate(json).get(doc) == simdjson::SUCCESS);

        JsonObject root;
        check(doc.get_object().get(root) == simdjson::SUCCESS);

        check(ExtractJsonFields(root, Field("array", [&](JsonArray arrayValue) {
            int sum = 0;
            int count = 0;
            for (auto item : arrayValue) {
                int value = 0;
                check(item.get_int32().get(value) == simdjson::SUCCESS);
                sum += value;
                ++count;
            }
            check(count == 3);
            check(sum == 6);
        })) == 1);
    }

    {
        simdjson::ondemand::parser parser;
        auto json = simdjson::padded_string(std::string_view(R"({"s":"hello"})"));
        simdjson::ondemand::document doc;
        check(parser.iterate(json).get(doc) == simdjson::SUCCESS);

        JsonObject root;
        check(doc.get_object().get(root) == simdjson::SUCCESS);

        std::string value;
        check(ExtractJsonFields(root, Field("s", value)) == 1);
        check(value == "hello");
    }

    {
        simdjson::ondemand::parser parser;
        auto json = simdjson::padded_string(std::string_view(R"({"s":"view"})"));
        simdjson::ondemand::document doc;
        check(parser.iterate(json).get(doc) == simdjson::SUCCESS);

        JsonObject root;
        check(doc.get_object().get(root) == simdjson::SUCCESS);

        std::string_view value;
        check(ExtractJsonFields(root, Field("s", value)) == 1);
        check(value == "view");
    }

    {
        simdjson::ondemand::parser parser;
        auto json = simdjson::padded_string(std::string_view(R"({"b":true})"));
        simdjson::ondemand::document doc;
        check(parser.iterate(json).get(doc) == simdjson::SUCCESS);

        JsonObject root;
        check(doc.get_object().get(root) == simdjson::SUCCESS);

        bool value = false;
        check(ExtractJsonFields(root, Field("b", value)) == 1);
        check(value);
    }

    {
        simdjson::ondemand::parser parser;
        auto json = simdjson::padded_string(std::string_view(R"({"i":-3})"));
        simdjson::ondemand::document doc;
        check(parser.iterate(json).get(doc) == simdjson::SUCCESS);

        JsonObject root;
        check(doc.get_object().get(root) == simdjson::SUCCESS);

        int value = 0;
        check(ExtractJsonFields(root, Field("i", value)) == 1);
        check(value == -3);
    }

    {
        simdjson::ondemand::parser parser;
        auto json = simdjson::padded_string(std::string_view(R"({"e":7})"));
        simdjson::ondemand::document doc;
        check(parser.iterate(json).get(doc) == simdjson::SUCCESS);

        JsonObject root;
        check(doc.get_object().get(root) == simdjson::SUCCESS);

        TestEnum value = TestEnum::None;
        check(ExtractJsonFields(root, Field("e", value)) == 1);
        check(value == TestEnum::Value);
    }

    {
        simdjson::ondemand::parser parser;
        auto json = simdjson::padded_string(std::string_view(R"({"f":1.25})"));
        simdjson::ondemand::document doc;
        check(parser.iterate(json).get(doc) == simdjson::SUCCESS);

        JsonObject root;
        check(doc.get_object().get(root) == simdjson::SUCCESS);

        float value = 0.0f;
        check(ExtractJsonFields(root, Field("f", value)) == 1);
        check(value == 1.25f);
    }

    {
        simdjson::ondemand::parser parser;
        auto json = simdjson::padded_string(std::string_view(
            R"({"obj":{"inner":9},"arr":[2,4,6],"s":"callback","b":true,"i":-5,"f":2.5,"ei":7})"));
        simdjson::ondemand::document doc;
        check(parser.iterate(json).get(doc) == simdjson::SUCCESS);

        JsonObject root;
        check(doc.get_object().get(root) == simdjson::SUCCESS);

        int objectInner = 0;
        int arraySum = 0;
        std::string str;
        bool booleanValue = false;
        int intValue = 0;
        float floatValue = 0.0f;
        TestEnum enumInvokedValue = TestEnum::None;

        int extracted = ExtractJsonFields(root,
            Field("obj", [&](JsonObject v) {
                check(ExtractJsonFields(v, Field("inner", objectInner)) == 1);
            }),
            Field("arr", [&](JsonArray v) {
                for (auto item : v) {
                    int tmp = 0;
                    check(item.get_int32().get(tmp) == simdjson::SUCCESS);
                    arraySum += tmp;
                }
            }),
            Field("s", [&](std::string_view v) { str = std::string(v); }),
            Field("b", [&](bool v) { booleanValue = v; }),
            Field("i", [&](int v) { intValue = v; }),
            Field("f", [&](float v) { floatValue = v; }),
            Field("ei", [&](TestEnum v) { enumInvokedValue = v; })
        );

        check(extracted == 7);
        check(objectInner == 9);
        check(arraySum == 12);
        check(str == "callback");
        check(booleanValue);
        check(intValue == -5);
        check(floatValue == 2.5f);
        check(enumInvokedValue == TestEnum::Value);
    }
}
