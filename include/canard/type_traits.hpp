#ifndef CANARD_TYPE_TRAITS_HPP
#define CANARD_TYPE_TRAITS_HPP

#include <type_traits>

namespace canard {

    template <class T>
    struct remove_cvref
        : std::remove_cv<typename std::remove_reference<T>::type>
    {
    };

    template <class T>
    using remove_cvref_t = typename remove_cvref<T>::type;

    template <class T, class U>
    struct is_same_value_type
        : std::is_same<remove_cvref_t<T>, remove_cvref_t<U>>
    {
    };

    template <class T, class... U>
    struct is_related : std::false_type {};

    template <class T, class U>
    struct is_related<T, U>
        : std::is_same<remove_cvref_t<T> , remove_cvref_t<U>>
    {};

} // namespace canard

#endif // CANARD_TYPE_TRAITS_HPP
