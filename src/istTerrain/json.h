#pragma once

#include <string_view>
#include <tuple>
#include <type_traits>
#include <functional>
#include <array>
#include <utility>
#include <simdjson/simdjson.h>

using JsonValue = simdjson::ondemand::value;
using JsonObject = simdjson::ondemand::object;
using JsonArray = simdjson::ondemand::array;

namespace detail {
    template<class T, class = void>
    struct unary_arg {
        using type = void;
    };

    template<class R, class A>
    struct unary_arg<R(*)(A), void> {
        using type = A;
    };

    template<class R, class A>
    struct unary_arg<R(&)(A), void> {
        using type = A;
    };

    template<class C, class R, class A>
    struct unary_arg<R(C::*)(A), void> {
        using type = A;
    };

    template<class C, class R, class A>
    struct unary_arg<R(C::*)(A) const, void> {
        using type = A;
    };

    template<class C, class R, class A>
    struct unary_arg<R(C::*)(A) noexcept, void> {
        using type = A;
    };

    template<class C, class R, class A>
    struct unary_arg<R(C::*)(A) const noexcept, void> {
        using type = A;
    };

    template<class T>
    struct unary_arg<T, std::void_t<decltype(&std::remove_reference_t<T>::operator())>>
        : unary_arg<decltype(&std::remove_reference_t<T>::operator())> {};

    template<class T>
    using unary_arg_t = typename unary_arg<std::remove_cvref_t<T>>::type;

    template<class T>
    inline constexpr bool is_unary_invocable_v =
        !std::is_void_v<unary_arg_t<T>> &&
        std::is_invocable_v<T, unary_arg_t<T>>;

    template<class T>
    inline constexpr bool is_enum_invocable_v =
        is_unary_invocable_v<T> &&
        std::is_enum_v<std::remove_cvref_t<unary_arg_t<T>>>;
} // namespace detail

template<class T>
inline decltype(auto) Field(std::string_view name, T&& value)
{
    if constexpr (detail::is_unary_invocable_v<T>) {
        return std::make_tuple(name, std::forward<T>(value));
    }
    else if constexpr (std::is_pointer_v<T>) {
        return std::make_tuple(name, value);
    }
    else {
        return std::make_tuple(name, &value);
    }
}

template<class... Args>
inline int ExtractJsonFields(JsonObject json, Args&&... args)
{
    auto handle = []<class T>(std::string_view k, JsonValue v, const std::tuple<std::string_view, T>&f) -> bool
    {
        if (k != std::get<0>(f))
            return false;

        using ValueType = std::remove_pointer_t<T>;
        if constexpr (std::is_same_v<T, std::string*>) {
            std::string_view tmp;
            if (v.get_string().get(tmp) == simdjson::SUCCESS) {
                *std::get<1>(f) = std::string(tmp);
                return true;
            }
        }
        else if constexpr (std::is_same_v<T, std::string_view*>) {
            if (v.get_string().get(*std::get<1>(f)) == simdjson::SUCCESS) {
                return true;
            }
        }
        else if constexpr (std::is_same_v<T, bool*>) {
            if (v.get_bool().get(*std::get<1>(f)) == simdjson::SUCCESS) {
                return true;
            }
        }
        else if constexpr (std::is_integral_v<ValueType> || std::is_enum_v<ValueType>) {
            int tmp = 0;
            if (v.get_int32().get(tmp) == simdjson::SUCCESS) {
                *std::get<1>(f) = static_cast<ValueType>(tmp);
                return true;
            }
        }
        else if constexpr (std::is_floating_point_v<ValueType>) {
            double tmp = 0.0;
            if (v.get_double().get(tmp) == simdjson::SUCCESS) {
                *std::get<1>(f) = static_cast<ValueType>(tmp);
                return true;
            }
        }
        //// ondemand だと parser が処理を進めるとそれ以前の JsonObject や JsonArray の内容が無効になるので、これらは invoker のみ許可する
        //else if constexpr (std::is_same_v<T, JsonObject*>) {
        //    if (v.get_object().get(*std::get<1>(f)) == simdjson::SUCCESS) {
        //        return true;
        //    }
        //}
        //else if constexpr (std::is_same_v<T, JsonArray*>) {
        //    if (v.get_array().get(*std::get<1>(f)) == simdjson::SUCCESS) {
        //        return true;
        //    }
        //}
        else if constexpr (detail::is_unary_invocable_v<T>) {
            using ArgType = std::remove_cvref_t<detail::unary_arg_t<T>>;

            if constexpr (std::is_same_v<ArgType, JsonObject>) {
                JsonObject tmp;
                if (v.get_object().get(tmp) == simdjson::SUCCESS) {
                    std::get<1>(f)(tmp);
                    return true;
                }
            }
            else if constexpr (std::is_same_v<ArgType, JsonArray>) {
                JsonArray tmp;
                if (v.get_array().get(tmp) == simdjson::SUCCESS) {
                    std::get<1>(f)(tmp);
                    return true;
                }
            }
            else if constexpr (std::is_same_v<ArgType, std::string_view>) {
                std::string_view tmp;
                if (v.get_string().get(tmp) == simdjson::SUCCESS) {
                    std::get<1>(f)(tmp);
                    return true;
                }
            }
            else if constexpr (std::is_same_v<ArgType, bool>) {
                bool tmp = false;
                if (v.get_bool().get(tmp) == simdjson::SUCCESS) {
                    std::get<1>(f)(tmp);
                    return true;
                }
            }
            else if constexpr (std::is_integral_v<ArgType> || std::is_enum_v<ArgType>) {
                int tmp = 0;
                if (v.get_int32().get(tmp) == simdjson::SUCCESS) {
                    std::get<1>(f)(static_cast<ArgType>(tmp));
                    return true;
                }
            }
            else if constexpr (std::is_floating_point_v<ArgType>) {
                double tmp = 0.0;
                if (v.get_double().get(tmp) == simdjson::SUCCESS) {
                    std::get<1>(f)(static_cast<ArgType>(tmp));
                    return true;
                }
            }
            else {
                static_assert(!sizeof(T), "Unsupported invocable argument type for JSON field extraction.");
            }
        }
        else {
            static_assert(!sizeof(T), "Unsupported type for JSON field extraction.");
        }
        return false;
    };

    auto handlers = std::make_tuple(std::forward<Args>(args)...);
    // 一度呼んだハンドラは以後呼ばないようにするためのフラグ
    std::array<bool, sizeof...(Args)> done{};

    std::string_view key;
    JsonValue val;
    int ret = 0;
    for (auto field : json) {
        if (field.unescaped_key().get(key) != simdjson::SUCCESS ||
            field.value().get(val) != simdjson::SUCCESS) {
            continue;
        }

        bool handled = false;
        [&] <size_t... I>(std::index_sequence<I...>) {
            ([&]() {
                if (!handled && !done[I] && handle(key, val, std::get<I>(handlers))) {
                    // ハンドリングされたのでフラグを立てて以後呼ばないようにする
                    handled = done[I] = true;
                    ++ret;
                }
                }(), ...);
        }(std::make_index_sequence<sizeof...(Args)>{});

        if (ret == sizeof...(Args)) {
            break;
        }
    }
    return ret;
}

template<class... Args>
inline int ExtractJsonFields(JsonValue json, Args&&... args)
{
    if (auto obj = json.get_object()) {
        return ExtractJsonFields(obj.value_unsafe(), std::forward<Args>(args)...);
    }
    return 0;
}
