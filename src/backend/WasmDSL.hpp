#pragma once

#include "backend/WebAssembly.hpp"
#include <concepts>
#include <cstdlib>
#include <deque>
#include <experimental/type_traits>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <mutable/util/concepts.hpp>
#include <mutable/util/fn.hpp>
#include <mutable/util/macro.hpp>
#include <mutable/util/tag.hpp>
#include <mutable/util/type_traits.hpp>
#include <tuple>
#include <type_traits>
#include <utility>
#include <x86intrin.h>

// Binaryen
#include <wasm-binary.h>
#include <wasm-builder.h>
#include <wasm-interpreter.h>
#include <wasm-validator.h>
#include <wasm.h>


namespace m {

// forward declarations
struct Table;

namespace wasm {

/*======================================================================================================================
 * Concepts needed for forward declarations
 *====================================================================================================================*/

/** Check whether \tparam T is a primitive type and not decayable. */
template<typename T>
concept dsl_primitive = primitive<T> and not decayable<T>;

/** Check whether \tparam T is a pointer to primitive type and not decayable. */
template<typename T>
concept dsl_pointer_to_primitive = pointer_to_primitive<T> and not decayable<T>;


/*======================================================================================================================
 * Type forward declarations
 *====================================================================================================================*/

/** Declares the kind of a variable: local, parameter, or global. */
enum class VariableKind {
    Local,
    Param,
    Global,
};

struct Allocator; // for use in Module
struct LocalBit; // for use in Module
struct LocalBitmap; // for use in Module
template<typename> struct FunctionProxy; // for use in Module
template<typename> struct invoke_v8; // for unittests only
template<typename> struct PrimitiveExpr;
template<typename> struct Expr;
template<typename, VariableKind, bool> struct Variable;
template<typename> struct Parameter;

namespace detail {

template<typename, VariableKind, bool> class variable_storage;

template<dsl_primitive, bool> struct the_reference;

}

template<typename T>
using Reference = detail::the_reference<T, false>;
template<typename T>
using ConstReference = detail::the_reference<T, true>;


/*======================================================================================================================
 * Concepts and meta types
 *====================================================================================================================*/

/** Helper type to deduce the `PrimitiveExpr<U>` type given a type \tparam T. */
template<typename T>
struct primitive_expr;

/** Specialization for reference type \tparam T. */
template<typename T>
requires requires { typename primitive_expr<T>::type; }
struct primitive_expr<T&>
{
    using type = typename primitive_expr<T>::type;
};

/** Specialization for decayable \tparam T. */
template<decayable T>
requires requires { typename primitive_expr<std::decay_t<T>>::type; }
struct primitive_expr<T>
{ using type = typename primitive_expr<std::decay_t<T>>::type; };

/** Specialization for primitive type \tparam T. */
template<dsl_primitive T>
struct primitive_expr<T>
{ using type = PrimitiveExpr<T>; };

/** Specialization for pointer to primitive type \tparam T. */
template<dsl_pointer_to_primitive T>
struct primitive_expr<T>
{ using type = PrimitiveExpr<T>; };

/** Specialization for \tparam T being `PrimitiveExpr<T>` already. */
template<typename T>
struct primitive_expr<PrimitiveExpr<T>>
{ using type = PrimitiveExpr<T>; };

/** Specialization for \tparam T being a `Variable<T, Kind, false>` (i.e. a `Variable` that *cannot* be `NULL`). */
template<typename T, VariableKind Kind>
struct primitive_expr<Variable<T, Kind, false>>
{ using type = PrimitiveExpr<T>; };

/** Specialization for \tparam T being a `Parameter` (i.e. a local `Variable` that *cannot* be `NULL`). */
template<typename T>
struct primitive_expr<Parameter<T>>
{ using type = PrimitiveExpr<T>; };

/** Specialization for \tparam T being a `the_reference`. */
template<typename T, bool IsConst>
struct primitive_expr<detail::the_reference<T, IsConst>>
{ using type = PrimitiveExpr<T>; };

/** Convenience alias for `primitive_expr`. */
template<typename T>
using primitive_expr_t = typename primitive_expr<T>::type;


/** Detects whether a type \tparam T is convertible to `PrimitiveExpr<U>`. */
template<typename T>
concept primitive_convertible = not pointer<T> and requires { typename primitive_expr_t<T>; };


/** Helper type to deduce the `Expr<U>` type given a \tparam T. */
template<typename T>
struct expr;

/** Specialization for reference type \tparam T. */
template<typename T>
requires requires { typename expr<T>::type; }
struct expr<T&>
{
    using type = typename expr<T>::type;
};

/** Specialization for decayable \tparam T. */
template<decayable T>
requires requires { typename expr<std::decay_t<T>>::type; }
struct expr<T>
{ using type = typename expr<std::decay_t<T>>::type; };

/** Specialization for primitive type \tparam T. */
template<dsl_primitive T>
struct expr<T>
{ using type = Expr<T>; };

/** Specialization for pointer to primitive type \tparam T. */
template<dsl_pointer_to_primitive T>
struct expr<T>
{ using type = Expr<T>; };

/** Specialization for \tparam T being `PrimitiveExpr<T>` already. */
template<typename T>
struct expr<PrimitiveExpr<T>>
{ using type = Expr<T>; };

/** Specialization for \tparam T being a `Parameter` (i.e. a local `Variable` that *cannot* be `NULL`). */
template<typename T>
struct expr<Parameter<T>>
{ using type = Expr<T>; };

/** Specialization for \tparam T being a `the_reference`. */
template<typename T, bool IsConst>
struct expr<detail::the_reference<T, IsConst>>
{ using type = Expr<T>; };

/** Specialization for \tparam T being `Expr<T>` already. */
template<typename T>
struct expr<Expr<T>>
{ using type = Expr<T>; };

/** Specialization for \tparam T being a `Variable<T, Kind, CanBeNull>`. */
template<typename T, VariableKind Kind, bool CanBeNull>
struct expr<Variable<T, Kind, CanBeNull>>
{ using type = Expr<T>; };

/** Convenience alias for `expr`. */
template<typename T>
using expr_t = typename expr<T>::type;


/** Detect whether a type \tparam T is convertible to `Expr<U>`. */
template<typename T>
concept expr_convertible = not pointer<T> and requires { typename expr_t<T>; };


namespace detail {

template<typename>
struct wasm_type_helper;

/** Converts a compile-time type \tparam T into a runtime-type `::wasm::Type`. */
template<typename T>
requires (std::is_void_v<T>)
struct wasm_type_helper<T>
{
    ::wasm::Type operator()() const { return ::wasm::Type(::wasm::Type::none); }
};

/** Specialization for \tparam T being integral. */
template<std::integral T>
struct wasm_type_helper<T>
{
    ::wasm::Type operator()() const {
        /* NOTE: there are no unsigned types, only unsigned operations */
        if constexpr (sizeof(T) <= 4)
            return ::wasm::Type(::wasm::Type::i32);
        if constexpr (sizeof(T) == 8)
            return ::wasm::Type(::wasm::Type::i64);
        M_unreachable("illegal type");
    };
};

/** Specialization for \tparam T being floating point. */
template<std::floating_point T>
struct wasm_type_helper<T>
{
    ::wasm::Type operator()()
    {
        if constexpr (sizeof(T) <= 4)
            return ::wasm::Type(::wasm::Type::f32);
        if constexpr (sizeof(T) == 8)
            return ::wasm::Type(::wasm::Type::f64);
        M_unreachable("illegal type");
    };
};

/** Specialization for \tparam T being pointer to primitive. */
template<dsl_pointer_to_primitive T>
struct wasm_type_helper<T>
{
    ::wasm::Type operator()() { return ::wasm::Type(::wasm::Type::i32); };
};

/** Specialization for \tparam T being function with parameters. */
template<typename ReturnType, typename... ParamTypes>
struct wasm_type_helper<ReturnType(ParamTypes...)>
{
    ::wasm::Signature operator()()
    {
        return ::wasm::Signature(
            /* params= */ { wasm_type_helper<ParamTypes>{}()... },
            /* result= */ wasm_type_helper<ReturnType>{}());
    };
};

}

template<typename T>
auto wasm_type() { return detail::wasm_type_helper<T>{}(); }



/*======================================================================================================================
 * Wasm_insist(COND [, MSG])
 *
 * Similarly to `M_insist()`, checks a condition in debug build and prints location information and an optional
 * message if it evaluates to `false`.  However, the condition is checked at runtime inside the Wasm code.
 *====================================================================================================================*/

#ifndef NDEBUG
#define WASM_INSIST2_(COND, MSG) m::wasm::Module::Get().emit_insist(COND, __FILE__, __LINE__, MSG)
#define WASM_INSIST1_(COND) WASM_INSIST2_(COND, nullptr)

#else
#define WASM_INSIST2_(COND, MSG) while (0) { ((void) (COND), (void) (MSG)); }
#define WASM_INSIST1_(COND) while (0) { ((void) (COND)); }

#endif

#define WASM_GET_INSIST_(XXX, _1, _2, NAME, ...) NAME
#define Wasm_insist(...) WASM_GET_INSIST_(XXX, ##__VA_ARGS__, WASM_INSIST2_, WASM_INSIST1_)(__VA_ARGS__)


/*######################################################################################################################
 * TYPE DEFINITIONS
 *####################################################################################################################*/

/*======================================================================================================================
 * Boxing types
 *====================================================================================================================*/

/** Stores the "branch targets" introduced by control flow structures, i.e. loops.
 *
 * The "break" target identifies the parent `::wasm::Block` of the loop to break out of. The "continue" target
 * identifies the `::wasm::Loop` to reiterate. */
struct branch_target_t
{
    ///> the break target
    ::wasm::Name brk;
    ///> the continue target
    ::wasm::Name continu;
    ///> the continue condition (may be `nullptr` if there is no condition)
    ::wasm::Expression *condition = nullptr;

    branch_target_t(::wasm::Name brk, ::wasm::Name continu, ::wasm::Expression *condition)
        : brk(brk), continu(continu), condition(condition)
    { }
};


/*======================================================================================================================
 * Helper functions
 *====================================================================================================================*/

/** A helper type to print the Wasm types for the types \tparam Ts. */
template<typename... Ts>
struct print_types;

/** Prints \tparam T, recurses to print \tparam Ts. */
template<typename T, typename... Ts>
struct print_types<T, Ts...>
{
    friend std::ostream & operator<<(std::ostream &out, print_types) {
        return out << wasm_type<T>() << ", " << print_types<Ts...>{};
    }
};

/** Prints the Wasm type for \tparam T. */
template<typename T>
struct print_types<T>
{
    friend std::ostream & operator<<(std::ostream &out, print_types) {
        return out << wasm_type<T>();
    }
};

/** Creates a unique name from a given \p prefix and a \p counter.  Increments `counter`. */
inline std::string unique(std::string prefix, unsigned &counter)
{
    static thread_local std::ostringstream oss;
    oss.str("");
    oss << prefix << '<' << counter++ << '>';
    return oss.str();
}

/** Creates a `::wasm::Literal` of type \tparam T from a given \p value.  Used to solve macOS ambiguity. */
template<typename T, typename U>
requires std::floating_point<T> and equally_floating<T, U>
inline ::wasm::Literal make_literal(U value)
{
    return ::wasm::Literal(T(value));
}

/** Creates a `::wasm::Literal` of type \tparam T from a given \p value.  Used to solve macOS ambiguity. */
template<typename T, typename U>
requires signed_integral<U> and equally_floating<T, U>
inline ::wasm::Literal make_literal(U value)
{
    return sizeof(T) <= 4 ? ::wasm::Literal(int32_t(value))
                          : ::wasm::Literal(int64_t(value));
}

/** Creates a `::wasm::Literal` of type \tparam T from a given \p value.  Used to solve macOS ambiguity. */
template<typename T, typename U>
requires (unsigned_integral<U> and equally_floating<T, U>) or boolean<U>
inline ::wasm::Literal make_literal(U value)
{
    return sizeof(T) <= 4 ? ::wasm::Literal(uint32_t(value))
                          : ::wasm::Literal(uint64_t(value));
}


/*======================================================================================================================
 * Exceptions
 *====================================================================================================================*/

#define M_EXCEPTION_LIST(X) \
    X(invalid_escape_sequence) \
    X(failed_unittest_check)

struct exception : backend_exception
{
#define DECLARE_ENUM(TYPE) TYPE,
    enum exception_t : uint64_t {
        M_EXCEPTION_LIST(DECLARE_ENUM)
    };
#undef DECLARE_ENUM

#define DECLARE_NAMES(TYPE) #TYPE,
    static constexpr const char * const names_[] = {
        M_EXCEPTION_LIST(DECLARE_NAMES)
    };
#undef DECLARE_NAMES

    private:
    exception_t type_;

    public:
    explicit exception(exception_t type, std::string message) : backend_exception(std::move(message)), type_(type) { }
};


/*======================================================================================================================
 * Callback functions
 *====================================================================================================================*/

/** Reports a runtime error.  The index to the filename, the line, and an optional message stored by the host is given
 * by `args`. */
::wasm::Literals insist_interpreter(::wasm::Literals &args);

/** Throws an exception.  The exception type id and the index to the filename, the line, and an optional message stored
 * by the host is given by `args`. */
::wasm::Literals throw_interpreter(::wasm::Literals &args);

const std::map<::wasm::Name, std::function<::wasm::Literals(::wasm::Literals&)>> callback_functions = {
#define CALLBACK(NAME, FUNC) { NAME, FUNC },
    CALLBACK("insist", insist_interpreter)
    CALLBACK("throw", throw_interpreter)
#undef CALLBACK
};


/*======================================================================================================================
 * GarbageCollectedData
 *====================================================================================================================*/

/** Helper struct for garbage collection done by the `Module`.  Inherit from this struct, provide a c`tor expecting
 * a `GarbageCollectedData&&` instance, and register the created struct in the module to garbage collect it
 * automatically when the module is destroyed. */
struct GarbageCollectedData
{
    friend struct Module;

    private:
    GarbageCollectedData() = default;

    public:
    GarbageCollectedData(GarbageCollectedData&&) = default;

    virtual ~GarbageCollectedData() { }
};


/*======================================================================================================================
 * Module
 *====================================================================================================================*/

struct Module final
{
    /*----- Friends --------------------------------------------------------------------------------------------------*/
    friend struct Block;
    friend struct BlockUser;
    template<typename> friend struct Function;
    template<typename> friend struct PrimitiveExpr;
    template<typename, VariableKind, bool> friend class detail::variable_storage;
    friend struct LocalBit;
    template<typename> friend struct invoke_v8;
    friend struct Allocator;

    private:
    ///> counter to make block names unique
    static inline std::atomic_uint NEXT_MODULE_ID_ = 0;

    ///> the unique ID for this `Module`
    unsigned id_;
    ///> counter to make block names unique
    unsigned next_block_id_ = 0;
    ///> counter to make function names unique
    unsigned next_function_id_ = 0;
    ///> counter to make global variable names unique
    unsigned next_global_id_ = 0;
    ///> counter to make if names unique
    unsigned next_if_id_ = 0;
    ///> counter to make loop names unique
    unsigned next_loop_id_ = 0;
    ///> the Binaryen Wasm module
    ::wasm::Module module_;
    ///> the Binaryen expression builder for the `module_`
    ::wasm::Builder builder_;
    ///> the currently active Binaryen block
    ::wasm::Block *active_block_ = nullptr;
    ///> the currently active Binaryen function
    ::wasm::Function *active_function_ = nullptr;
    ///> the main memory of the module
    ::wasm::Memory *memory_ = nullptr;
    ///> the allocator
    std::unique_ptr<Allocator> allocator_;
    ///> stack of Binaryen branch targets
    std::vector<branch_target_t> branch_target_stack_;
    ///> filename, line, and an optional message for each emitted insist or exception throw
    std::vector<std::tuple<const char*, unsigned, const char*>> messages_;
    ///> this module's interface, if any
    std::unique_ptr<::wasm::ModuleRunner::ExternalInterface> interface_;
    ///> the per-function stacks of local bitmaps; used for local boolean variables and NULL bits
    std::vector<std::vector<LocalBitmap*>> local_bitmaps_stack_;
    ///> mapping from handles to garbage collected data
    std::unordered_map<void*, std::unique_ptr<GarbageCollectedData>> garbage_collected_data_;

    /*----- Thread-local instance ------------------------------------------------------------------------------------*/
    private:
    static thread_local std::unique_ptr<Module> the_module_;

    Module();
    Module(const Module&) = delete;

    public:
    static void Init() {
        M_insist(not the_module_, "must not have a module yet");
        the_module_ = std::unique_ptr<Module>(new Module());
    }
    static void Dispose() {
        M_insist(bool(the_module_), "must have a module");
        the_module_ = nullptr;
    }
    static Module & Get() {
        M_insist(bool(the_module_), "must have a module");
        return *the_module_;
    }

    /*----- Access methods -------------------------------------------------------------------------------------------*/
    /** Returns the ID of the current module. */
    static unsigned ID() { return Get().id_; }

    /** Returns a unique block name in the current module. */
    static std::string Unique_Block_Name(std::string prefix = "block") { return unique(prefix, Get().next_block_id_); }
    /** Returns a unique function name in the current module. */
    static std::string Unique_Function_Name(std::string prefix = "function") {
        return unique(prefix, Get().next_function_id_);
    }
    /** Returns a unique global name in the current module. */
    static std::string Unique_Global_Name(std::string prefix = "global") {
        return unique(prefix, Get().next_global_id_);
    }
    /** Returns a unique if name in the current module. */
    static std::string Unique_If_Name(std::string prefix = "if") { return unique(prefix, Get().next_if_id_); }
    /** Returns a unique loop name in the current module. */
    static std::string Unique_Loop_Name(std::string prefix = "loop") { return unique(prefix, Get().next_loop_id_); }

    /** Returns the expression builder of the current module. */
    static ::wasm::Builder & Builder() { return Get().builder_; }

    /** Returns the currently active block. */
    static ::wasm::Block & Block() { return *M_notnull(Get().active_block_); }

    /** Returns the currently active function. */
    static ::wasm::Function & Function() { return *M_notnull(Get().active_function_); }

    /** Returns the allocator. */
    static Allocator & Allocator();

    /** Validates that the module is well-formed. */
    static bool Validate(bool verbose = true, bool global = true);

    /** Optimizes the module with the optimization level set to `level`. */
    static void Optimize(int optimization_level);

    /** Sets the new active `::wasm::Block` and returns the previously active `::wasm::Block`. */
    ::wasm::Block * set_active_block(::wasm::Block *block) { return std::exchange(active_block_, block); }
    /** Sets the new active `::wasm::Function` and returns the previously active `::wasm::Function`. */
    ::wasm::Function * set_active_function(::wasm::Function *fn) { return std::exchange(active_function_, fn); }

    /*----- Control flow ---------------------------------------------------------------------------------------------*/
    /** An unsafe, i.e. statically-**un**typed, version of `Function::emit_return()`. */
    void emit_return();
    /** An unsafe, i.e. statically-**un**typed, version of `Function::emit_return(T&&)`. */
    template<typename T>
    void emit_return(PrimitiveExpr<T> expr);
    /** An unsafe, i.e. statically-**un**typed, version of `Function::emit_return(T&&)`. */
    template<typename T>
    void emit_return(Expr<T> expr);

    void emit_break(std::size_t level = 1);
    void emit_break(PrimitiveExpr<bool> cond, std::size_t level = 1);

    void emit_continue(std::size_t level = 1);
    void emit_continue(PrimitiveExpr<bool> cond, std::size_t level = 1);

    template<typename T>
    PrimitiveExpr<T> emit_select(PrimitiveExpr<bool> cond, PrimitiveExpr<T> tru, PrimitiveExpr<T> fals);
    template<typename T>
    Expr<T> emit_select(PrimitiveExpr<bool> cond, Expr<T> tru, Expr<T> fals);

    /*----- Globals. -------------------------------------------------------------------------------------------------*/
    template<dsl_primitive T, dsl_primitive U>
    requires requires (U u) { make_literal<T>(u); }
    void emit_global(::wasm::Name name, U init = U(), bool is_mutable = true) {
        ::wasm::Builder::Mutability mut = is_mutable ? ::wasm::Builder::Mutability::Mutable
                                                     : ::wasm::Builder::Mutability::Immutable;
        ::wasm::Const *_init = builder_.makeConst(make_literal<T>(init));
        auto global = builder_.makeGlobal(name, wasm_type<T>(), _init, mut);
        module_.addGlobal(std::move(global));
    }
    template<dsl_pointer_to_primitive T>
    void emit_global(::wasm::Name name, uint32_t init = 0, bool is_mutable = true) {
        ::wasm::Builder::Mutability mut = is_mutable ? ::wasm::Builder::Mutability::Mutable
                                                     : ::wasm::Builder::Mutability::Immutable;
        ::wasm::Const *_init = builder_.makeConst(::wasm::Literal(init));
        auto global = builder_.makeGlobal(name, wasm_type<T>(), _init, mut);
        module_.addGlobal(std::move(global));
    }

    template<typename T>
    PrimitiveExpr<T> get_global(const char *name);

    /*----- Imports & Exports ----------------------------------------------------------------------------------------*/
    template<typename T>
    requires dsl_primitive<T> or dsl_pointer_to_primitive<T>
    void emit_import(const char *extern_name, const char *intern_name = nullptr)
    {
        ::wasm::Const *value;
        if constexpr (std::is_pointer_v<T>)
            value = builder_.makeConst(::wasm::Literal(0));
        else
            value = builder_.makeConst(::wasm::Literal(T()));
        auto global = builder_.makeGlobal(intern_name ? intern_name : extern_name, wasm_type<T>(), M_notnull(value),
                                          ::wasm::Builder::Mutability::Immutable);
        global->module = "imports";
        global->base = extern_name;
        module_.addGlobal(std::move(global));
    }

    /** Add function `name` with type `T` as import. */
    template<typename T>
    void emit_function_import(const char *name) {
        auto func = module_.addFunction(builder_.makeFunction(name, wasm_type<T>(), {}));
        func->module = "imports";
        func->base = name;
    }

    /** Add function `name` with type `T` as export. */
    void emit_function_export(const char *name) {
        module_.addExport(builder_.makeExport(name, name, ::wasm::ExternalKind::Function));
    }

    /*----- Function calls -------------------------------------------------------------------------------------------*/
    template<typename ReturnType, typename... ParamTypes>
    requires std::is_void_v<ReturnType>
    void emit_call(const char *fn, PrimitiveExpr<ParamTypes>... args);

    template<typename ReturnType, typename... ParamTypes>
    requires dsl_primitive<ReturnType> or dsl_pointer_to_primitive<ReturnType>
    PrimitiveExpr<ReturnType> emit_call(const char *fn, PrimitiveExpr<ParamTypes>... args);

    /*----- Runtime checks and throwing exceptions -------------------------------------------------------------------*/
    void emit_insist(PrimitiveExpr<bool> cond, const char *filename, unsigned line, const char *msg);

    void emit_throw(exception::exception_t type, const char *filename, unsigned line, const char *msg);

    const std::tuple<const char*, unsigned, const char*> & get_message(std::size_t idx) const {
        return messages_.at(idx);
    }

    /*----- Garbage collected data -----------------------------------------------------------------------------------*/
    /** Adds and returns an instance of \tparam C, which will be created by calling its c`tor with an
     * `GarbageCollectedData&&` instance and the forwarded \p args, to `this` `Module`s garbage collection using the
     * unique caller handle \p handle. */
    template<class C, typename... Args>
    C & add_garbage_collected_data(void *handle, Args... args) {
        auto it = garbage_collected_data_.template try_emplace(
            /* key=   */ handle,
            /* value= */ std::make_unique<C>(GarbageCollectedData(), std::forward<Args>(args)...)
        ).first;
        return as<C>(*it->second);
    }

    /*----- Interpretation & Debugging -------------------------------------------------------------------------------*/
    ::wasm::ModuleRunner::ExternalInterface * get_mock_interface();

    /** Create an instance of this module.  Can be used for interpretation and debugging. */
    ::wasm::ModuleRunner instantiate() { return ::wasm::ModuleRunner(module_, get_mock_interface()); }

    /*----- Module settings ------------------------------------------------------------------------------------------*/
    void set_feature(::wasm::FeatureSet feature, bool value) { module_.features.set(feature, value); }

    /** Returns the binary representation of `module_` in a freshly allocated memory.  The caller must dispose of this
     * memory. */
    std::pair<uint8_t*, std::size_t> binary();

    private:
    void create_local_bitmap_stack();
    void dispose_local_bitmap_stack();
    public:
    LocalBit allocate_bit();

    void push_branch_targets(::wasm::Name brk, ::wasm::Name continu) {
        branch_target_stack_.emplace_back(brk, continu, nullptr);
    }

    void push_branch_targets(::wasm::Name brk, ::wasm::Name continu, PrimitiveExpr<bool> condition);

    branch_target_t pop_branch_targets() {
        auto top = branch_target_stack_.back();
        branch_target_stack_.pop_back();
        return top;
    }

    const branch_target_t & current_branch_targets() const { return branch_target_stack_.back(); }

    /*----- Printing -------------------------------------------------------------------------------------------------*/
    public:
    friend std::ostream & operator<<(std::ostream &out, const Module &M) {
        out << "Module\n";

        out << "  currently active block: ";
        if (M.active_block_) {
            if (M.active_block_->name.is())
                out << '"' << M.active_block_->name << '"';
            else
                out << "<anonymous block>";
        } else {
            out << "none";
        }
        out << '\n';

        // out << "  currently active function: ";
        // if (M.active_function_) {
        //     out << '"' << M.active_function_->name << '"';
        // } else {
        //     out << "none";
        // }
        // out << '\n';

        return out;
    }

    void dump(std::ostream &out) const { out << *this << std::endl; }
    void dump() const { dump(std::cerr); }
};


/*======================================================================================================================
 * Block
 *====================================================================================================================*/

/** Represents a code block, i.e. a sequential sequence of code.  Necessary to compose conditional control flow and
 * useful for simultaneous code generation at several locations.  */
struct Block final
{
    template<typename T> friend struct Function; // to get ::wasm::Block of body
    friend struct BlockUser; // to access `Block` internals
    friend struct If; // to get ::wasm::Block for *then* and *else* part
    friend struct Loop; // to get ::wasm::Block for loop body
    friend struct DoWhile; // to get ::wasm::Block for loop body

    private:
    ///> this block, can be `nullptr` if default-constructed or the block has already been attached
    ::wasm::Block *this_block_ = nullptr;
    ///> the parent block, before this block was created
    ::wasm::Block *parent_block_ = nullptr;
    ///> whether this block attaches itself to its parent block
    bool attach_to_parent_ = false;

    public:
    friend void swap(Block &first, Block &second) {
        using std::swap;
        swap(first.this_block_,       second.this_block_);
        swap(first.parent_block_,     second.parent_block_);
        swap(first.attach_to_parent_, second.attach_to_parent_);
    }

    private:
    Block() = default;

    /** Create a new `Block` for a given `::wasm::Block` and set it *active* in the current `Module`. */
    Block(::wasm::Block *block, bool attach_to_parent)
        : this_block_(M_notnull(block))
        , attach_to_parent_(attach_to_parent)
    {
        if (attach_to_parent_) {
            parent_block_ = Module::Get().active_block_;
            M_insist(not attach_to_parent_ or parent_block_, "can only attach to parent if there is a parent block");
        }
    }

    public:
    /** Create an anonymous `Block`. */
    explicit Block(bool attach_to_parent) : Block(Module::Builder().makeBlock(), attach_to_parent) { }
    /** Create a named `Block` and set it *active* in the current `Module`. */
    explicit Block(std::string name, bool attach_to_parent)
        : Block(Module::Builder().makeBlock(Module::Unique_Block_Name(name)), attach_to_parent)
    { }
    /** Create a named `Block` and set it *active* in the current `Module`. */
    explicit Block(const char *name, bool attach_to_parent) : Block(std::string(name), attach_to_parent) { }

    Block(const Block&) = delete;
    Block(Block &&other) : Block() { swap(*this, other); }

    ~Block() {
        if (this_block_ and attach_to_parent_)
            attach_to(*M_notnull(parent_block_));
    }

    Block & operator=(Block &&other) { swap(*this, other); return *this; }

    private:
    ::wasm::Block & get() const { return *M_notnull(this_block_); }
    ::wasm::Block & previous() const { return *M_notnull(parent_block_); }

    void attach_to(::wasm::Block &other) {
        other.list.push_back(this_block_);
        this_block_ = nullptr;
    }

    public:
    bool has_name() const { return get().name; }
    std::string name() const { M_insist(has_name()); return get().name.toString(); }

    /** Returns whether this `Block` is empty, i.e. contains to expressions. */
    bool empty() const { return this_block_->list.empty(); }

    /** Attaches this `Block` to the given `Block` \p other. */
    void attach_to(Block &other) {
        M_insist(not attach_to_parent_, "cannot explicitly attach if attach_to_parent is true");
        attach_to(*M_notnull(other.this_block_));
    }

    /** Attaches this `Block` to the `wasm::Block` currently active in the `Module`. */
    void attach_to_current() {
        M_insist(not attach_to_parent_, "cannot explicitly attach if attach_to_parent is true");
        attach_to(Module::Block());
    }

    friend std::ostream & operator<<(std::ostream &out, const Block &B) {
        out << "vvvvvvvvvv block";
        if (B.has_name())
            out << " \"" << B.name() << '"';
        out << " starts here vvvvvvvvvv\n";

        for (auto expr : B.get().list)
            out << *expr << '\n';

        out << "^^^^^^^^^^^ block";
        if (B.has_name())
            out << " \"" << B.name() << '"';
        out << " ends here ^^^^^^^^^^^\n";

        return out;
    }

    void dump(std::ostream &out) const { out << *this; out.flush(); }
    void dump() const { dump(std::cerr); }
};

/** A helper class to *use* a `Block`, thereby setting the `Block` active for code generation.  When the `BlockUser` is
 * destructed, restores the previously active block for code generation. */
struct BlockUser
{
    private:
    const Block &block_; ///< the block to use (for code gen)
    ::wasm::Block *old_block_ = nullptr; ///< the previously active, now old block

    public:
    BlockUser(const Block &block) : block_(block) {
        old_block_ = Module::Get().set_active_block(block_.this_block_); // set active block
    }

    ~BlockUser() { Module::Get().set_active_block(old_block_); } // restore previously active block
};


/*======================================================================================================================
 * Function
 *====================================================================================================================*/

/** Represents a Wasm function.  It is templated with return type and parameter types.  This enables us to access
 * parameters with their proper types.  */
template<typename>
struct Function;

template<typename ReturnType, typename... ParamTypes>
struct Function<PrimitiveExpr<ReturnType>(PrimitiveExpr<ParamTypes>...)>
{
    template<typename> friend struct FunctionProxy;

    ///> the type of the function
    using type = ReturnType(ParamTypes...);
    ///> the return type of the function
    using return_type = ReturnType;
    ///> the amount of parameters of the function
    static constexpr std::size_t PARAMETER_COUNT = sizeof...(ParamTypes);

    /*------------------------------------------------------------------------------------------------------------------
     * Parameter helper types
     *----------------------------------------------------------------------------------------------------------------*/
    private:
    template<typename... Ts, std::size_t... Is>
    requires (sizeof...(Ts) == sizeof...(Is))
    std::tuple<Parameter<Ts>...>
    make_parameters_helper(std::index_sequence<Is...>)
    {
        return std::make_tuple<Parameter<Ts>...>(
            (Parameter<Ts>(Is), ...)
        );
    }

    /** Creates a `std::tuple` with statically typed fields, one per parameter. */
    template<typename... Ts, typename Indices = std::make_index_sequence<sizeof...(Ts)>>
    std::tuple<Parameter<Ts>...>
    make_parameters()
    {
        return make_parameters_helper<Ts...>(Indices{});
    }

    /** Provides an alias `type` with the type of the \tparam I -th parameter. */
    template<std::size_t I, typename... Ts>
    struct parameter_type;
    template<std::size_t I, typename T, typename... Ts>
    struct parameter_type<I, T, Ts...>
    {
        static_assert(I <= sizeof...(Ts), "parameter index out of range");
        using type = typename parameter_type<I - 1, Ts...>::type;
    };
    template<typename T, typename... Ts>
    struct parameter_type<0, T, Ts...>
    {
        using type = T;
    };
    /** Convenience alias for `parameter_type::type`. */
    template<std::size_t I>
    using parameter_type_t = typename parameter_type<I, ParamTypes...>::type;

    private:
    ::wasm::Name name_; ///< the *unique* name of this function
    Block body_; ///< the function body
    ///> the `::wasm::Function` implementing this function
    ::wasm::Function *this_function_ = nullptr;
    ///> the previously active `::wasm::Function` (may be `nullptr`)
    ::wasm::Function *previous_function_ = nullptr;

    public:
    friend void swap(Function &first, Function &second) {
        using std::swap;
        swap(first.name_,              second.name_);
        swap(first.body_,              second.body_);
        swap(first.this_function_,     second.this_function_);
        swap(first.previous_function_, second.previous_function_);
    }

    private:
    Function() = default;

    public:
    /** Constructs a fresh `Function` and expects a unique \p name.  To be called by `FunctionProxy`. */
    Function(const std::string &name)
        : name_(name)
        , body_(name + ".body", /* attach_to_parent= */ false)
    {
        /*----- Set block return type for non-`void` functions. -----*/
        if constexpr (not std::is_void_v<ReturnType>)
            body_.get().type = wasm_type<ReturnType>();

        /*----- Create Binaryen function. -----*/
        auto fn = Module::Builder().makeFunction(
            /* name= */ name,
            /* type= */ wasm_type<ReturnType(ParamTypes...)>(),
            /* vars= */ std::vector<::wasm::Type>{}
        );
        fn->body = &body_.get(); // set function body
        this_function_ = Module::Get().module_.addFunction(std::move(fn));
        M_insist(this_function_->getNumParams() == sizeof...(ParamTypes));
        Module::Get().create_local_bitmap_stack();

        /*----- Set this function active in the `Module`. -----*/
        previous_function_ = Module::Get().set_active_function(this_function_);
    }

    Function(const Function&) = delete;
    Function(Function &&other) : Function() { swap(*this, other); }

    ~Function() {
        if constexpr (not std::is_void_v<ReturnType>)
            body_.get().list.push_back(Module::Builder().makeUnreachable());
        Module::Get().dispose_local_bitmap_stack();
        /*----- Restore previously active function in the `Module`. -----*/
        Module::Get().set_active_function(previous_function_);
    }

    Function & operator=(Function &&other) { swap(*this, other); return *this; }

    public:
    /** Returns the body of this function. */
    Block & body() { return body_; }
    /** Returns the body of this function. */
    const Block & body() const { return body_; }

    /** Returns the name of this function. */
    std::string name() const { return name_.toString(); }

    /** Returns all parameters of this function. */
    std::tuple<Parameter<ParamTypes>...> parameters() { return make_parameters<ParamTypes...>(); }

    /** Returns the \tparam I -th parameter, statically typed via `parameter_type_t`. */
    template<std::size_t I>
    Parameter<parameter_type_t<I>>
    parameter() { return Parameter<parameter_type_t<I>>(I); }

    /** Emits a return instruction returning `void`. */
    void emit_return()
    requires std::is_void_v<ReturnType>
    { Module::Block().list.push_back(Module::Builder().makeReturn()); }

    /** Emits a return instruction returning `PrimitiveExpr<T>` constructed from \p t of type \tparam T. */
    template<primitive_convertible T>
    requires (not std::is_void_v<ReturnType>) and
    requires (T &&t) { static_cast<primitive_expr_t<ReturnType>>(primitive_expr_t<T>(std::forward<T>(t))); }
    void emit_return(T &&t)
    {
        primitive_expr_t<ReturnType> value(primitive_expr_t<T>(std::forward<T>(t)));
        Module::Get().emit_return(value);
    }

    /** Emits a return instruction returning `PrimitiveExpr<T>` constructed from \p t of type \tparam T. Checks that \t
     * t is `NOT NULL`. */
    template<expr_convertible T>
    requires (not std::is_void_v<ReturnType>) and (not primitive_convertible<T>) and
    requires (T t) { static_cast<expr_t<ReturnType>>(expr_t<T>(t)); } // ad-hoc constraint
    void emit_return(T &&t)
    {
        expr_t<ReturnType> expr(expr_t<T>(std::forward<T>(t)));
        Module::Get().emit_return(expr);
    }

    private:
    ::wasm::Function & get() const { return *M_notnull(this_function_); }

    public:
    friend std::ostream & operator<<(std::ostream &out, const Function &Fn) {
        out << "function \"" << Fn.name() << "\" : ";
        if constexpr (PARAMETER_COUNT)
            out << print_types<ParamTypes...>{};
        else
            out << typeid(void).name();
        out << " -> " << print_types<ReturnType>{} << '\n';

        if (not Fn.get().vars.empty()) {
            out << "  " << Fn.get().getNumVars() << " local variables:";
            for (::wasm::Index i = 0, end = Fn.get().getNumVars(); i != end; ++i)
                out << " [" << i << "] " << Fn.get().vars[i];
            out << '\n';
        }

        out << Fn.body();
        return out;
    }

    void dump(std::ostream &out) const { out << *this; out.flush(); }
    void dump() const { dump(std::cerr); }
};

template<typename ReturnType, typename... ParamTypes>
struct Function<ReturnType(ParamTypes...)> : Function<PrimitiveExpr<ReturnType>(PrimitiveExpr<ParamTypes>...)>
{
    using Function<PrimitiveExpr<ReturnType>(PrimitiveExpr<ParamTypes>...)>::Function;
};


/*======================================================================================================================
 * FunctionProxy
 *====================================================================================================================*/

/** A handle to create a `Function` and to create invocations of that function. Provides `operator()()` to emit a
 * function call by issuing a C-style call.  The class is template typed with the function signature, allowing us to
 * perform static type checking of arguments and the returned value at call sites. */
template<typename ReturnType, typename... ParamTypes>
struct FunctionProxy<PrimitiveExpr<ReturnType>(PrimitiveExpr<ParamTypes>...)>
{
    using type = ReturnType(ParamTypes...);

    private:
    std::string name_; ///< the unique name of the `Function`

    public:
    FunctionProxy() = delete;
    FunctionProxy(std::string name) : name_(Module::Unique_Function_Name(name)) { }
    FunctionProxy(const char *name) : FunctionProxy(std::string(name)) { }

    FunctionProxy(FunctionProxy&&) = default;

    FunctionProxy & operator=(FunctionProxy&&) = default;

    const std::string & name() const { return name_; }
    const char * c_name() const { return name_.c_str(); }

    Function<type> make_function() const { return Function<type>(name_); }

    /*----- Overload operator() to emit function calls ---------------------------------------------------------------*/
    /** Call function returning `void` with parameters \p args of types \tparam Args. */
    template<typename... Args>
    requires requires (Args&&... args) { (PrimitiveExpr<ParamTypes>(std::forward<Args>(args)),...); }
    void operator()(Args&&... args) const
    requires std::is_void_v<ReturnType>
    { operator()(PrimitiveExpr<ParamTypes>(std::forward<Args>(args))...); }

    /** Call function returning `void` with parameters \p args of `PrimitiveExpr` type. */
    void operator()(PrimitiveExpr<ParamTypes>... args) const
    requires std::is_void_v<ReturnType>
    {
        Module::Block().list.push_back(
            Module::Builder().makeCall(name_, { args.expr()... }, wasm_type<ReturnType>())
        );
    }

    /** Call function returning non-`void` with parameters \p args of types \tparam Args. */
    template<typename... Args>
    requires requires (Args&&... args) { (PrimitiveExpr<ParamTypes>(std::forward<Args>(args)),...); }
    PrimitiveExpr<ReturnType> operator()(Args&&... args) const
    requires (not std::is_void_v<ReturnType>)
    { return operator()(PrimitiveExpr<ParamTypes>(std::forward<Args>(args))...); }

    /** Call function returning non-`void` with parameters \p args of `PrimitiveExpr` type. */
    PrimitiveExpr<ReturnType> operator()(PrimitiveExpr<ParamTypes>... args) const
    requires (not std::is_void_v<ReturnType>)
    {
        return PrimitiveExpr<ReturnType>(
            Module::Builder().makeCall(name_, { args.expr()... }, wasm_type<ReturnType>())
        );
    }
};

template<typename ReturnType, typename... ParamTypes>
struct FunctionProxy<ReturnType(ParamTypes...)> : FunctionProxy<PrimitiveExpr<ReturnType>(PrimitiveExpr<ParamTypes>...)>
{
    using FunctionProxy<PrimitiveExpr<ReturnType>(PrimitiveExpr<ParamTypes>...)>::FunctionProxy;
};


/*======================================================================================================================
 * PrimitiveExpr
 *====================================================================================================================*/

template<typename T, typename U>
concept arithmetically_combinable = primitive_convertible<T> and primitive_convertible<U> and have_common_type<T, U> and
requires (primitive_expr_t<T> v) { primitive_expr_t<common_type_t<T, U>>(v); } and
requires (primitive_expr_t<U> v) { primitive_expr_t<common_type_t<T, U>>(v); };

/** Specialization of `PrimitiveExpr<T>` for primitive type \tparam T.  Represents an expression (AST) evaluating to a
 * runtime value of primitive type \tparam T. */
template<dsl_primitive T>
struct PrimitiveExpr<T>
{
    ///> the primitive type of the represented expression
    using type = T;

    /*----- Friends --------------------------------------------------------------------------------------------------*/
    template<typename> friend struct PrimitiveExpr; // to convert U to T and U* to uint32_t
    template<typename> friend struct Expr; // to construct an empty `PrimitiveExpr<bool>` for the NULL information
    template<typename, VariableKind, bool>
    friend class detail::variable_storage; // to construct from `::wasm::Expression` and access private `expr()`
    friend struct Module; // to access internal `::wasm::Expression`, e.g. in `emit_return()`
    template<typename> friend struct FunctionProxy; // to access internal `::wasm::Expr` to construct function calls
    friend struct If; // to use PrimitiveExpr<bool> as condition
    friend struct While; // to use PrimitiveExpr<bool> as condition

    private:
    ///> the referenced Binaryen expression (AST)
    ::wasm::Expression *expr_ = nullptr;
    ///> a list of referenced `LocalBit`s
    std::list<std::shared_ptr<LocalBit>> referenced_bits_;

    private:
    ///> Constructs an empty `PrimitiveExpr`, for which `operator bool()` returns `false`.
    explicit PrimitiveExpr() = default;

    ///> Constructs a `PrimitiveExpr` from a Binaryen `::wasm::Expression` \p expr and the \p referenced_bits.
    explicit PrimitiveExpr(::wasm::Expression *expr, std::list<std::shared_ptr<LocalBit>> referenced_bits = {})
        : expr_(expr)
        , referenced_bits_(std::move(referenced_bits))
    { }
    /** Constructs a `PrimitiveExpr` from a `std::pair` of a Binaryen `::wasm::Expression` \p expr and the \p
     * referenced_bits. */
    explicit PrimitiveExpr(std::pair<::wasm::Expression*, std::list<std::shared_ptr<LocalBit>>> expr)
        : PrimitiveExpr(std::move(expr.first), std::move(expr.second))
    { }

    public:
    /** Constructs a new `PrimitiveExpr` from a constant \p value. */
    template<dsl_primitive U>
    requires requires { make_literal<T>(U()); }
    explicit PrimitiveExpr(U value)
        : PrimitiveExpr(Module::Builder().makeConst(make_literal<T>(value)))
    { }

    /** Constructs a new `PrimitiveExpr` from a decayable constant \p value. */
    template<decayable U>
    requires dsl_primitive<std::decay_t<U>> and
    requires (U value) { PrimitiveExpr(std::decay_t<U>(value)); }
    explicit PrimitiveExpr(U value)
        : PrimitiveExpr(std::decay_t<U>(value))
    { }

    PrimitiveExpr(const PrimitiveExpr&) = delete;
    /** Constructs a new `PrimitiveExpr<T>` by **moving** the underlying `expr_` and `referenced_bits_` of `other`
     * to `this`. */
    PrimitiveExpr(PrimitiveExpr &other)
        : PrimitiveExpr(std::exchange(other.expr_, nullptr), std::move(other.referenced_bits_))
    { /* move, not copy */ }
    /** Constructs a new `PrimitiveExpr<T>` by **moving** the underlying `expr_` and `referenced_bits_` of `other`
     * to `this`. */
    PrimitiveExpr(PrimitiveExpr &&other)
        : PrimitiveExpr(std::exchange(other.expr_, nullptr), std::move(other.referenced_bits_))
    { }

    PrimitiveExpr & operator=(PrimitiveExpr&&) = delete;

    ~PrimitiveExpr() { M_insist(not expr_, "expression must be used or explicitly discarded"); }

    private:
    /** **Moves** the underlying Binaryen `::wasm::Expression` out of `this`. */
    ::wasm::Expression * expr() {
        M_insist(expr_, "cannot access an already moved or discarded expression of a `PrimitiveExpr`");
        return std::exchange(expr_, nullptr);
    }
    /** **Moves** the referenced bits out of `this`. */
    std::list<std::shared_ptr<LocalBit>> referenced_bits() { return std::move(referenced_bits_); }
    /** **Moves** the underlying Binaryen `::wasm::Expression` and the referenced bits out of `this`. */
    std::pair<::wasm::Expression*, std::list<std::shared_ptr<LocalBit>>> move() {
        return { expr(), referenced_bits() };
    }

    public:
    /** Returns `true` if this `PrimitiveExpr` actually holds a value (Binaryen AST), `false` otherwise. Can be used to
     * test whether this `PrimitiveExpr` has already been used. */
    explicit operator bool() const { return expr_ != nullptr; }

    /** Creates and returns a *deep copy* of `this`. */
    PrimitiveExpr clone() const {
        M_insist(expr_, "cannot clone an already moved or discarded `PrimitiveExpr`");
        return PrimitiveExpr(
            /* expr=            */ ::wasm::ExpressionManipulator::copy(expr_, Module::Get().module_),
            /* referenced_bits= */ referenced_bits_ // copy
        );
    }

    /** Discards `this`.  This is necessary to signal in our DSL that a value is *expectedly* unused (and not dead
     * code). For example, the return value of a function that was invoked because of its side effects may remain
     * unused.  One **must** discard the returned value to signal that the value is expectedly left unused. */
    void discard() {
        M_insist(expr_, "cannot discard an already moved or discarded `PrimitiveExpr`");
        if (expr_->is<::wasm::Call>())
            Module::Block().list.push_back(Module::Builder().makeDrop(expr_)); // keep the function call
#ifndef NDEBUG
        expr_ = nullptr;
#endif
        referenced_bits_.clear();
    }


    /*------------------------------------------------------------------------------------------------------------------
     * Operation helper
     *----------------------------------------------------------------------------------------------------------------*/

    private:
    /** Helper function to implement *unary* operations.  Applies `::wasm::UnaryOp` \p op to `this` and returns the
     * result. */
    template<dsl_primitive ResultType>
    PrimitiveExpr<ResultType> unary(::wasm::UnaryOp op) {
        M_insist(expr_, "PrimitiveExpr already moved or discarded");
        return PrimitiveExpr<ResultType>(
            /* expr=            */ Module::Builder().makeUnary(op, expr()),
            /* referenced_bits= */ referenced_bits() // moved
        );
    }

    /** Helper function to implement *binary* operations.  Applies `::wasm::BinaryOp` \p op to `this` and \p other and
     * returns the result.  Note, that we require `this` and \p other to be of same type. */
    template<dsl_primitive ResultType, dsl_primitive OperandType>
    PrimitiveExpr<ResultType> binary(::wasm::BinaryOp op, PrimitiveExpr<OperandType> other) {
        M_insist(this->expr_, "PrimitiveExpr already moved or discarded");
        M_insist(other.expr_, "PrimitiveExpr already moved or discarded");
        auto referenced_bits = this->referenced_bits(); // moved
        referenced_bits.splice(referenced_bits.end(), other.referenced_bits());
        return PrimitiveExpr<ResultType>(
            /* expr=            */ Module::Builder().makeBinary(op,
                                                                this->template to<OperandType>().expr(),
                                                                other.template to<OperandType>().expr()),
            /* referenced_bits= */ std::move(referenced_bits)
        );
    }


    /*------------------------------------------------------------------------------------------------------------------
     * Conversion operations
     *----------------------------------------------------------------------------------------------------------------*/

    private:
    template<dsl_primitive U>
    PrimitiveExpr<U> convert() {
        using From = T;
        using To = U;

        if constexpr (std::same_as<From, To>)
            return *this;
        if constexpr (integral<From> and integral<To> and std::is_signed_v<From> == std::is_signed_v<To> and
                      sizeof(From) == sizeof(To))
            return PrimitiveExpr<To>(move());

        if constexpr (boolean<From>) {                                                                  // from boolean
            if constexpr (integral<To>) {                                                               //  to integer
                if constexpr (sizeof(To) <= 4)                                                          //   bool -> i32
                    return PrimitiveExpr<To>(move());
                if constexpr (sizeof(To) == 8)                                                          //   bool -> i64
                    return unary<To>(::wasm::ExtendUInt32);
            }
            if constexpr (std::floating_point<To>) {                                                    //  to floating point
                if constexpr (sizeof(To) <= 4)                                                          //   bool -> f32
                    return unary<To>(::wasm::ConvertUInt32ToFloat32);
                if constexpr (sizeof(To) == 8)                                                          //   bool -> f64
                    return unary<To>(::wasm::ConvertUInt32ToFloat64);
            }
        }

        if constexpr (boolean<To>)                                                                      // to boolean
            return *this != PrimitiveExpr(static_cast<From>(0));

        if constexpr (integral<From>) {                                                                 // from integer
            if constexpr (integral<To>) {                                                               //  to integer
                if constexpr (std::is_signed_v<From>) {                                                 //   signed
                    if constexpr (sizeof(From) <= 4 and sizeof(To) == 8)                                //    i32 -> i64
                        return unary<To>(::wasm::ExtendSInt32);
                    if constexpr (sizeof(From) == 8 and sizeof(To) == 4)                                //    i64 -> i32
                        return unary<To>(::wasm::WrapInt64);
                } else {                                                                                //   unsigned
                    if constexpr (sizeof(From) <= 4 and sizeof(To) == 8)                                //    u32 -> u64
                        return unary<To>(::wasm::ExtendUInt32);
                    if constexpr (sizeof(From) == 8 and sizeof(To) == 4)                                //    u64 -> u32
                        return unary<To>(::wasm::WrapInt64);
                }
                if constexpr (sizeof(To) <= 4 and sizeof(From) < sizeof(To))                            //   From less precise than To
                    return PrimitiveExpr<To>(move());                                                   //    extend integer
                if constexpr (sizeof(From) <= 4 and sizeof(To) < sizeof(From)) {                        //   To less precise than From
                    constexpr From MASK = (uint64_t(1) << (8 * sizeof(To))) - uint64_t(1);
                    return PrimitiveExpr<To>((*this bitand PrimitiveExpr(MASK)).move());                //    truncate integer
                }
                if constexpr (sizeof(From) == 8 and sizeof(To) < 4) {                                   //   To less precise than From
                    if constexpr (std::is_signed_v<To>) {
                        auto wrapped = unary<int32_t>(::wasm::WrapInt64);                               //    wrap integer
                        constexpr int32_t MASK = (int64_t(1) << (8 * sizeof(To))) - int64_t(1);
                        return PrimitiveExpr<To>((wrapped bitand PrimitiveExpr<int32_t>(MASK)).move()); //    truncate integer
                    } else {
                        auto wrapped = unary<uint32_t>(::wasm::WrapInt64);                              //    wrap integer
                        constexpr uint32_t MASK = (uint64_t(1) << (8 * sizeof(To))) - uint64_t(1);
                        return PrimitiveExpr<To>((wrapped bitand PrimitiveExpr<uint32_t>(MASK)).move());//    truncate integer
                    }
                }
            }
            if constexpr (std::floating_point<To>) {                                                    //  to floating point
                if constexpr (std::is_signed_v<From>) {                                                 //   signed
                    if constexpr (sizeof(From) <= 4 and sizeof(To) == 4)                                //    i32 -> f32
                        return unary<To>(::wasm::ConvertSInt32ToFloat32);
                    if constexpr (sizeof(From) <= 4 and sizeof(To) == 8)                                //    i32 -> f64
                        return unary<To>(::wasm::ConvertSInt32ToFloat64);
                    if constexpr (sizeof(From) == 8 and sizeof(To) == 4)                                //    i64 -> f32
                        return unary<To>(::wasm::ConvertSInt64ToFloat32);
                    if constexpr (sizeof(From) == 8 and sizeof(To) == 8)                                //    i64 -> f64
                        return unary<To>(::wasm::ConvertSInt64ToFloat64);
                } else {                                                                                //   unsigned
                    if constexpr (sizeof(From) <= 4 and sizeof(To) == 4)                                //    u32 -> f32
                        return unary<To>(::wasm::ConvertUInt32ToFloat32);
                    if constexpr (sizeof(From) <= 4 and sizeof(To) == 8)                                //    u32 -> f64
                        return unary<To>(::wasm::ConvertUInt32ToFloat64);
                    if constexpr (sizeof(From) == 8 and sizeof(To) == 4)                                //    u64 -> f32
                        return unary<To>(::wasm::ConvertUInt64ToFloat32);
                    if constexpr (sizeof(From) == 8 and sizeof(To) == 8)                                //    u64 -> f64
                        return unary<To>(::wasm::ConvertUInt64ToFloat64);
                }
            }
        }

        if constexpr (std::floating_point<From>) {                                                      // from floating point
            if constexpr (integral<To>) {                                                               //  to integer
                if constexpr (std::is_signed_v<To>) {                                                   //   signed
                    if constexpr (sizeof(From) == 4 and sizeof(To) <= 4)                                //    f32 -> i32
                        return unary<int32_t>(::wasm::TruncSFloat32ToInt32).template to<To>();
                    if constexpr (sizeof(From) == 4 and sizeof(To) == 8)                                //    f32 -> i64
                        return unary<To>(::wasm::TruncSFloat32ToInt64);
                    if constexpr (sizeof(From) == 8 and sizeof(To) <= 4)                                //    f64 -> i32
                        return unary<int32_t>(::wasm::TruncSFloat64ToInt32).template to<To>();
                    if constexpr (sizeof(From) == 8 and sizeof(To) == 8)                                //    f64 -> i64
                        return unary<To>(::wasm::TruncSFloat64ToInt64);
                } else {                                                                                //   unsigned
                    if constexpr (sizeof(From) == 4 and sizeof(To) <= 4)                                //    f32 -> u32
                        return unary<int32_t>(::wasm::TruncUFloat32ToInt32).template to<To>();
                    if constexpr (sizeof(From) == 4 and sizeof(To) == 8)                                //    f32 -> u64
                        return unary<To>(::wasm::TruncUFloat32ToInt64);
                    if constexpr (sizeof(From) == 8 and sizeof(To) <= 4)                                //    f64 -> u32
                        return unary<int32_t>(::wasm::TruncUFloat64ToInt32).template to<To>();
                    if constexpr (sizeof(From) == 8 and sizeof(To) == 8)                                //    f64 -> u64
                        return unary<To>(::wasm::TruncUFloat64ToInt64);
                }
            }
            if constexpr (std::floating_point<To>) {                                                    //  to floating point
                if constexpr (sizeof(From) == 4 and sizeof(To) == 8)                                    //    f32 -> f64
                    return unary<To>(::wasm::PromoteFloat32);
                if constexpr (sizeof(From) == 8 and sizeof(To) == 4)                                    //    f64 -> f32
                    return unary<To>(::wasm::DemoteFloat64);
            }
        }

        M_unreachable("illegal conversion");
    }

    public:
    /** Implicit conversion of a `PrimitiveExpr<T>` to a `PrimitiveExpr<To>`.  Only applicable if
     *
     * - `T` and `To` have same signedness
     * - neither or both `T` and `To` are integers
     * - `T` can be *trivially* converted to `To` (e.g. `int` to `long` but not `long` to `int`)
     * - `To` is not `bool`
     */
    template<dsl_primitive To>
    requires same_signedness<T, To> and         // T and To have same signedness
             (integral<T> == integral<To>) and  // neither nor both T and To are integers (excluding bool)
             (sizeof(T) <= sizeof(To))          // T can be *trivially* converted to To
    operator PrimitiveExpr<To>() { return convert<To>(); }

    /** Explicit conversion of a `PrimitiveExpr<T>` to a `PrimitiveExpr<To>`.  Only applicable if
     *
     * - `T` and `To` have same signedness or `T` is `bool` or `char` or `To` is `bool` or `char`
     * - `T` can be converted to `To` (e.g. `int` to `long`, `long` to `int`, `float` to `int`)
     */
    template<dsl_primitive To>
    requires (same_signedness<T, To> or                     // T and To have same signedness
              boolean<T> or std::same_as<T, char> or        //  or T is bool
              boolean<To> or std::same_as<To, char>) and    //  or To is bool
             std::is_convertible_v<T, To>                   // and T can be converted to To (e.g. float -> int)
    PrimitiveExpr<To> to() { return convert<To>(); }

    /** Explicit conversion of a `PrimitiveExpr<uint32_t>` to a `PrimitiveExpr<T*>`
     *
     * - `T` is `uint32_t`
     * - `To` is a pointer to primitive type
     */
    template<dsl_pointer_to_primitive To>
    PrimitiveExpr<To> to()
    requires std::same_as<T, uint32_t>
    { return PrimitiveExpr<To>(*this); }

    /** Conversion of a `PrimitiveExpr<T>` to a `PrimitiveExpr<std::make_signed_t<T>>`. Only applicable if
     *
     * - `T` is an unsigned integral type except `bool`
     */
    auto make_signed() requires unsigned_integral<T>
    { return PrimitiveExpr<std::make_signed_t<T>>(move()); }

    /** Conversion of a `PrimitiveExpr<T>` to a `PrimitiveExpr<std::make_unsigned_t<T>>`. Only available if
    *
    * - `T` is a signed integral type except `bool`
    */
    auto make_unsigned() requires signed_integral<T>
    { return PrimitiveExpr<std::make_unsigned_t<T>>(move()); }


    /*------------------------------------------------------------------------------------------------------------------
     * Unary operations
     *----------------------------------------------------------------------------------------------------------------*/

#define UNOP_(NAME, SIGN, TYPE) (::wasm::UnaryOp::NAME##SIGN##TYPE)
#define UNIOP_(NAME, SIGN) [] { \
    if constexpr (sizeof(T) == 8) \
        return UNOP_(NAME,SIGN,Int64); \
    else \
        return UNOP_(NAME,SIGN,Int32); \
} ()
#define UNFOP_(NAME) [] { \
    if constexpr (sizeof(T) == 8) \
        return UNOP_(NAME,,Float64); \
    else \
        return UNOP_(NAME,,Float32); \
} ()

    /*----- Arithmetical operations ----------------------------------------------------------------------------------*/
    PrimitiveExpr operator+() requires arithmetic<T> { return *this; }

    PrimitiveExpr operator-() requires integral<T> { return PrimitiveExpr(T(0)) - *this; }
    PrimitiveExpr operator-() requires std::floating_point<T> { return unary<type>(UNFOP_(Neg)); }

    PrimitiveExpr abs() requires std::floating_point<T> { return unary<type>(UNFOP_(Abs)); }
    PrimitiveExpr ceil() requires std::floating_point<T> { return unary<type>(UNFOP_(Ceil)); }
    PrimitiveExpr floor() requires std::floating_point<T> { return unary<type>(UNFOP_(Floor)); }
    PrimitiveExpr sqrt() requires std::floating_point<T> { return unary<type>(UNFOP_(Sqrt)); }

    /*----- Bitwise operations ---------------------------------------------------------------------------------------*/

    PrimitiveExpr operator~() requires integral<T> { return PrimitiveExpr(T(-1)) ^ *this; }

    PrimitiveExpr clz() requires unsigned_integral<T> and (sizeof(T) >= 4) { return unary<type>(UNIOP_(Clz,)); }
    PrimitiveExpr clz() requires unsigned_integral<T> and (sizeof(T) == 2) {
        return unary<type>(UNIOP_(Clz,)) - PrimitiveExpr(16U); // the value is represented as I32
    }
    PrimitiveExpr clz() requires unsigned_integral<T> and (sizeof(T) == 1) {
        return unary<type>(UNIOP_(Clz,)) - PrimitiveExpr(24U); // the value is represented as I32
    }
    PrimitiveExpr ctz() requires unsigned_integral<T> { return unary<type>(UNIOP_(Ctz,)); }
    PrimitiveExpr popcnt() requires unsigned_integral<T> { return unary<type>(UNIOP_(Popcnt,)); }

    /*----- Comparison operations ------------------------------------------------------------------------------------*/

    PrimitiveExpr<bool> eqz() requires integral<T>
    { return unary<bool>(UNIOP_(EqZ,)); }

    /*----- Logical operations ---------------------------------------------------------------------------------------*/

    PrimitiveExpr<bool> operator not() requires std::same_as<T, bool> { return unary<bool>(UNIOP_(EqZ,)); }

    /*----- Hashing operations ---------------------------------------------------------------------------------------*/

    PrimitiveExpr<uint64_t> hash() requires unsigned_integral<T> { return *this; }
    PrimitiveExpr<uint64_t> hash() requires signed_integral<T> { return make_unsigned(); }
    PrimitiveExpr<uint64_t> hash() requires std::floating_point<T> { return to<int64_t>().make_unsigned(); }
    PrimitiveExpr<uint64_t> hash() requires std::same_as<T, bool> { return to<uint64_t>(); }

#undef UNFOP_
#undef UNIOP_
#undef UNOP_


    /*------------------------------------------------------------------------------------------------------------------
     * Binary operations
     *----------------------------------------------------------------------------------------------------------------*/

#define BINOP_(NAME, SIGN, TYPE) (::wasm::BinaryOp::NAME##SIGN##TYPE)
#define BINIOP_(NAME, SIGN) [] { \
    if constexpr (sizeof(To) == 8) \
        return BINOP_(NAME,SIGN,Int64); \
    else \
        return BINOP_(NAME,SIGN,Int32); \
} ()
#define BINFOP_(NAME) [] { \
    if constexpr (sizeof(To) == 8) \
        return BINOP_(NAME,,Float64); \
    else \
        return BINOP_(NAME,,Float32); \
} ()
#define BINARY_OP(NAME, SIGN) [] { \
    if constexpr (std::is_integral_v<To>) { \
        return BINIOP_(NAME, SIGN); \
    } \
    if constexpr (std::is_floating_point_v<To>) { \
        return BINFOP_(NAME); \
    } \
    M_unreachable("unsupported operation"); \
} ()

    /*----- Arithmetical operations ----------------------------------------------------------------------------------*/

    /** Adds `this` to \p other. */
    template<arithmetic U>
    requires arithmetically_combinable<T, U>
    auto operator+(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>> {
        using To = common_type_t<T, U>;
        return binary<To, To>(BINARY_OP(Add,), other);
    }

    /** Subtracts \p other from `this`. */
    template<arithmetic U>
    requires same_signedness<T, U> and arithmetically_combinable<T, U>
    auto operator-(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>> {
        using To = common_type_t<T, U>;
        return binary<To, To>(BINARY_OP(Sub,), other);
    }

    /** Multiplies `this` and \p other. */
    template<arithmetic U>
    requires arithmetically_combinable<T, U>
    auto operator*(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>> {
        using To = common_type_t<T, U>;
        return binary<To, To>(BINARY_OP(Mul,), other);
    }

    /** Divides `this` by \p other. */
    template<arithmetic U>
    requires same_signedness<T, U> and arithmetically_combinable<T, U>
    auto operator/(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>> {
        using To = common_type_t<T, U>;
        if constexpr (std::is_signed_v<T>)
            return binary<To, To>(BINARY_OP(Div, S), other);
        else
            return binary<To, To>(BINARY_OP(Div, U), other);
    }

    /** Computes the remainder of dividing `this` by \p other. */
    template<integral U>
    requires same_signedness<T, U> and arithmetically_combinable<T, U>
    auto operator%(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>>
    requires integral<T>
    {
        using To = common_type_t<T, U>;
        if constexpr (std::is_signed_v<T>)
            return binary<To, To>(BINIOP_(Rem, S), other);
        else
            return binary<To, To>(BINIOP_(Rem, U), other);
    }

    /** Copy the sign bit of \p other to `this`. */
    template<std::floating_point U>
    requires arithmetically_combinable<T, U>
    auto copy_sign(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>>
    requires std::floating_point<T>
    {
        using To = common_type_t<T, U>;
        return binary<To, To>(BINFOP_(CopySign), other);
    }

    /** Computes the minimum of `this` and \p other. */
    template<std::floating_point U>
    requires arithmetically_combinable<T, U>
    auto min(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>>
    requires std::floating_point<T>
    {
        using To = common_type_t<T, U>;
        return binary<To, To>(BINFOP_(Min), other);
    }

    /** Computes the maximum of `this` and \p other. */
    template<std::floating_point U>
    requires arithmetically_combinable<T, U>
    auto max(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>>
    requires std::floating_point<T>
    {
        using To = common_type_t<T, U>;
        return binary<To, To>(BINFOP_(Max), other);
    }

    /*----- Bitwise operations ---------------------------------------------------------------------------------------*/

    /** Computes the bitwise *and* of `this` and \p other. */
    template<std::integral U>
    requires arithmetically_combinable<T, U>
    auto operator bitand(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>>
    requires std::integral<T>
    {
        using To = common_type_t<T, U>;
        return binary<To, To>(BINIOP_(And,), other);
    }

    /** Computes the bitwise *or* of `this` and \p other. */
    template<std::integral U>
    requires arithmetically_combinable<T, U>
    auto operator bitor(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>>
    requires std::integral<T>
    {
        using To = common_type_t<T, U>;
        return binary<To, To>(BINIOP_(Or,), other);
    }

    /** Computes the (bitwise) *xor* of `this` and \p other. */
    template<std::integral U>
    requires arithmetically_combinable<T, U>
    auto operator xor(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>>
    requires std::integral<T>
    {
        using To = common_type_t<T, U>;
        return binary<To, To>(BINIOP_(Xor,), other);
    }

    /** Shifts `this` *left* by \p other. */
    template<integral U>
    requires arithmetically_combinable<T, U>
    auto operator<<(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>>
    requires integral<T>
    {
        using To = common_type_t<T, U>;
        if constexpr (sizeof(To) >= 4)
            return binary<To, To>(BINIOP_(Shl,), other);
        else if constexpr (sizeof(To) == 2)
            return binary<To, To>(BINIOP_(Shl,), other) bitand PrimitiveExpr<To>(0xffff);
        else
            return binary<To, To>(BINIOP_(Shl,), other) bitand PrimitiveExpr<To>(0xff);
    }

    /** Shifts `this` *right* by \p other. */
    template<integral U>
    requires arithmetically_combinable<T, U>
    auto operator>>(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>>
    requires integral<T>
    {
        using To = common_type_t<T, U>;
        if constexpr (std::is_signed_v<T>)
            return binary<To, To>(BINIOP_(Shr, S), other);
        else
            return binary<To, To>(BINIOP_(Shr, U), other);
    }

    /** Rotates `this` *left* by \p other. */
    template<integral U>
    requires arithmetically_combinable<T, U> and (sizeof(common_type_t<T, U>) >= 4)
    auto rotl(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>>
    requires integral<T>
    {
        using To = common_type_t<T, U>;
        return binary<To, To>(BINIOP_(RotL,), other);
    }

    /** Rotates `this` *right* by \p other. */
    template<integral U>
    requires arithmetically_combinable<T, U> and (sizeof(common_type_t<T, U>) >= 4)
    auto rotr(PrimitiveExpr<U> other) -> PrimitiveExpr<common_type_t<T, U>>
    requires integral<T>
    {
        using To = common_type_t<T, U>;
        return binary<To, To>(BINIOP_(RotR,), other);
    }

    /*----- Comparison operations ------------------------------------------------------------------------------------*/

    /** Checks whether `this` equals \p other. */
    template<dsl_primitive U>
    requires same_signedness<T, U> and arithmetically_combinable<T, U>
    PrimitiveExpr<bool> operator==(PrimitiveExpr<U> other) {
        using To = common_type_t<T, U>;
        return binary<bool, To>(BINARY_OP(Eq,), other);
    }

    /** Checks whether `this` unequal to \p other. */
    template<dsl_primitive U>
    requires same_signedness<T, U> and arithmetically_combinable<T, U>
    PrimitiveExpr<bool> operator!=(PrimitiveExpr<U> other) {
        using To = common_type_t<T, U>;
        return binary<bool, To>(BINARY_OP(Ne,), other);
    }

    /** Checks whether `this` less than \p other. */
    template<arithmetic U>
    requires same_signedness<T, U> and arithmetically_combinable<T, U>
    PrimitiveExpr<bool> operator<(PrimitiveExpr<U> other)
    requires arithmetic<T>
    {
        using To = common_type_t<T, U>;
        if constexpr (std::is_signed_v<T>)
            return binary<bool, To>(BINARY_OP(Lt, S), other);
        else
            return binary<bool, To>(BINARY_OP(Lt, U), other);
    }

    /** Checks whether `this` less than or equals to \p other. */
    template<arithmetic U>
    requires same_signedness<T, U> and arithmetically_combinable<T, U>
    PrimitiveExpr<bool> operator<=(PrimitiveExpr<U> other)
    requires arithmetic<T>
    {
        using To = common_type_t<T, U>;
        if constexpr (std::is_signed_v<T>)
            return binary<bool, To>(BINARY_OP(Le, S), other);
        else
            return binary<bool, To>(BINARY_OP(Le, U), other);
    }

    /** Checks whether `this` greater than to \p other. */
    template<arithmetic U>
    requires same_signedness<T, U> and arithmetically_combinable<T, U>
    PrimitiveExpr<bool> operator>(PrimitiveExpr<U> other)
    requires arithmetic<T>
    {
        using To = common_type_t<T, U>;
        if constexpr (std::is_signed_v<T>)
            return binary<bool, To>(BINARY_OP(Gt, S), other);
        else
            return binary<bool, To>(BINARY_OP(Gt, U), other);
    }

    /** Checks whether `this` greater than or equals to \p other. */
    template<arithmetic U>
    requires same_signedness<T, U> and arithmetically_combinable<T, U>
    PrimitiveExpr<bool> operator>=(PrimitiveExpr<U> other)
    requires arithmetic<T>
    {
        using To = common_type_t<T, U>;
        if constexpr (std::is_signed_v<T>)
            return binary<bool, To>(BINARY_OP(Ge, S), other);
        else
            return binary<bool, To>(BINARY_OP(Ge, U), other);
    }

    /*----- Logical operations ---------------------------------------------------------------------------------------*/

    /** Computes the logical conjunction (`and`) of `this` and \p other. */
    template<boolean U>
    PrimitiveExpr<bool>
    operator and(PrimitiveExpr<U> other)
    requires boolean<T>
    {
        return binary<bool, bool>(BINOP_(And,,Int32), other);
    }

    /** Computes the logical disjunction (`or`) of `this` and \p other. */
    template<boolean U>
    PrimitiveExpr<bool>
    operator or(PrimitiveExpr<U> other)
    requires boolean<T>
    {
        return binary<bool, bool>(BINOP_(Or,,Int32), other);
    }

#undef BINARY_OP
#undef BINFOP_
#undef BINIOP_
#undef BINOP_


    /*------------------------------------------------------------------------------------------------------------------
     * Printing
     *----------------------------------------------------------------------------------------------------------------*/

    friend std::ostream & operator<<(std::ostream &out, const PrimitiveExpr &P) {
        out << "PrimitiveExpr<" << typeid(type).name() << ">: ";
        if (P.value_) out << *P.value_;
        else          out << "None";
        return out;
    }

    void dump(std::ostream &out) const { out << *this << std::endl; }
    void dump() const { dump(std::cerr); }
};

/*======================================================================================================================
 * Define binary operators on `PrimitiveExpr`
 *====================================================================================================================*/

/** List of supported binary operators on `PrimitiveExpr`, `Expr`, `Variable`, etc. */
#define BINARY_LIST(X) \
    X(operator +) \
    X(operator -) \
    X(operator *) \
    X(operator /) \
    X(operator %) \
    X(operator bitand) \
    X(operator bitor) \
    X(operator xor) \
    X(operator <<) \
    X(operator >>) \
    X(operator ==) \
    X(operator !=) \
    X(operator <) \
    X(operator <=) \
    X(operator >) \
    X(operator >=) \
    X(operator and) \
    X(operator or) \
    X(copy_sign) \
    X(min) \
    X(max) \
    X(rotl) \
    X(rotr)

/*----- Forward binary operators on operands convertible to PrimitiveExpr<T> -----------------------------------------*/
#define MAKE_BINARY(OP) \
    template<primitive_convertible T, primitive_convertible U> \
    requires requires (primitive_expr_t<T> t, primitive_expr_t<U> u) { t.OP(u); } \
    auto OP(T &&t, U &&u) { \
        return primitive_expr_t<T>(std::forward<T>(t)).OP(primitive_expr_t<U>(std::forward<U>(u))); \
    }
BINARY_LIST(MAKE_BINARY)
#undef MAKE_BINARY

/** Specialization of `PrimitiveExpr<T>` for pointer to primitive type \tparam T.  Represents an expression (AST)
 * evaluating to a runtime value of pointer to primitive type \tparam T. */
template<dsl_pointer_to_primitive T>
struct PrimitiveExpr<T>
{
    using type = T;
    using pointed_type = std::decay_t<std::remove_pointer_t<T>>;
    using offset_t = int32_t;

    /*----- Friends --------------------------------------------------------------------------------------------------*/
    template<typename> friend struct PrimitiveExpr; // to convert U* to T* and to convert uint32_t to T*
    template<typename, VariableKind, bool>
    friend class detail::variable_storage; // to construct from `::wasm::Expression` and access private `expr()`
    friend struct Module; // to acces internal ::wasm::Expr
    template<typename> friend struct FunctionProxy; // to access internal `::wasm::Expr` to construct function calls
    template<dsl_primitive, bool> friend struct detail::the_reference; // to access load()/store()

    private:
    PrimitiveExpr<uint32_t> addr_; ///< the address into the Wasm linear memory
    offset_t offset_ = 0; ///< offset to this in bytes; used to directly address pointer via base address and offset

    public:
    /** Constructs a `PrimitiveExpr<T*>` from the memory address \p addr.  Optionally accepts an \p offset. */
    explicit PrimitiveExpr(PrimitiveExpr<uint32_t> addr, offset_t offset = 0) : addr_(addr), offset_(offset) { }

    private:
    /** Constructs a `PrimitiveExpr<T*>` from the given address \p addr and \p referenced_bits.  Optionally accepts an
     * \p offset. */
    explicit PrimitiveExpr(::wasm::Expression *addr, std::list<std::shared_ptr<LocalBit>> referenced_bits = {},
                           offset_t offset = 0)
        : addr_(addr, std::move(referenced_bits))
        , offset_(offset)
    { }
    /** Constructs a `PrimitiveExpr<T*>` from a `std::pair` \p addr of the addres and the shared bits.  Optionally
     * accepts an \p offset. */
    explicit PrimitiveExpr(std::pair<::wasm::Expression*, std::list<std::shared_ptr<LocalBit>>> addr,
                           offset_t offset = 0)
        : PrimitiveExpr(std::move(addr.first), std::move(addr.second), offset)
    { }

    public:
    PrimitiveExpr(const PrimitiveExpr&) = delete;
    /** Constructs a new `PrimitiveExpr<T*>` by **moving** the underlying `expr_`, `referenced_bits`, and `offset_`
     * of `other` to `this`. */
    PrimitiveExpr(PrimitiveExpr &other) : addr_(other.addr_), offset_(other.offset_) { /* move, not copy */ }
    /** Constructs a new `PrimitiveExpr<T*>` by **moving** the underlying `expr_`, `referenced_bits`, and `offset_`
     * of `other` to `this`. */
    PrimitiveExpr(PrimitiveExpr &&other) : addr_(other.addr_), offset_(other.offset_) { }

    PrimitiveExpr & operator=(PrimitiveExpr&&) = delete;

    /** Constructs a Wasm `nullptr`.  Note, that in order to implement `nullptr` in Wasm, we must create an artificial
     * address that cannot be accessed. */
    static PrimitiveExpr Nullptr() { return PrimitiveExpr(PrimitiveExpr<uint32_t>(0U)); }

    private:
    /** **Moves** the underlying Binaryen `::wasm::Expression` out of `this`. */
    ::wasm::Expression * expr() { return to<uint32_t>().expr(); }
    /** **Moves** the underlying Binaryen `wasm::Expression` and the referenced bits out of `this`. */
    std::pair<::wasm::Expression*, std::list<std::shared_ptr<LocalBit>>> move() { return addr_.move(); }

    public:
    /** Returns `true` if this `PrimitiveExpr` actually holds a value (Binaryen AST), `false` otherwise. Can be used to
     * test whether this `PrimitiveExpr` has already been used. */
    explicit operator bool() const { return bool(addr_); }

    /** Creates and returns a *deep copy* of `this`. */
    PrimitiveExpr clone() const { return PrimitiveExpr(addr_.clone(), offset_); }

    /** Discards `this`.  This is necessary to signal in our DSL that a value is *expectedly* unused (and not dead
     * code). For example, the return value of a function that was invoked because of its side effects may remain
     * unused.  One **must** discard the returned value to signal that the value is expectedly left unused. */
    void discard() { addr_.discard(); }


    /*------------------------------------------------------------------------------------------------------------------
     * Conversion operations
     *----------------------------------------------------------------------------------------------------------------*/

    public:
    /** Explicit conversion of a `PrimitiveExpr<void*>` to a `PrimitiveExpr<To>`.  Only applicable if \tparam To is a
     * pointer to primitive type. */
    template<dsl_pointer_to_primitive To>
    requires (not std::is_void_v<std::remove_pointer_t<To>>)
    PrimitiveExpr<To> to()
    requires std::is_void_v<pointed_type>
    {
        Wasm_insist((clone().template to<uint32_t>() % uint32_t(alignof(std::remove_pointer_t<To>))).eqz(),
                    "cannot convert to type whose alignment requirement is not fulfilled");
        return PrimitiveExpr<To>(addr_.move(), offset_);
    }

    /** Explicit conversion of a `PrimitiveExpr<T*>` to a `PrimitiveExpr<uint32_t>`.  Adds possible offset to
     * the pointer. */
    template<typename To>
    requires std::same_as<To, uint32_t>
    PrimitiveExpr<uint32_t> to()
    { return offset_ ? (offset_ > 0 ? addr_ + uint32_t(offset_) : addr_ - uint32_t(-offset_)) : addr_; }

    /** Explicit conversion of a `PrimitiveExpr<T*>` to a `PrimitiveExpr<void*>`. */
    template<typename To>
    requires (not std::same_as<To, T>) and std::same_as<To, void*>
    PrimitiveExpr<void*> to()
    { return PrimitiveExpr<void*>(addr_.move(), offset_); }

    /** Explicit dummy conversion of a `PrimitiveExpr<T*>` to a `PrimitiveExpr<T*>`.  Only needed for convenience
     * reasons, i.e. to match behaviour of `PrimitiveExpr<dsl_primitive>`. */
    template<typename To>
    requires std::same_as<To, T>
    PrimitiveExpr to() { return *this; }


    /*------------------------------------------------------------------------------------------------------------------
     * Hashing operations
     *----------------------------------------------------------------------------------------------------------------*/

    PrimitiveExpr<uint64_t> hash() { return to<uint32_t>().hash(); }


    /*------------------------------------------------------------------------------------------------------------------
     * Pointer operations
     *----------------------------------------------------------------------------------------------------------------*/

    public:
    /** Evaluates to `true` if `this` is `nullptr`. */
    PrimitiveExpr<bool> is_nullptr() { return to<uint32_t>() == 0U; }

    /** Returnes a `std::pair` of `this` and a `PrimitiveExpr<bool>` that tells whether `this` is `nullptr`. */
    std::pair<PrimitiveExpr<type>, PrimitiveExpr<bool>> split() { auto cpy = clone(); return { cpy, is_nullptr() }; }

    /** Dereferencing a pointer `PrimitiveExpr<T*>` yields a `Reference<T>`. */
    auto operator*() requires dsl_primitive<pointed_type> {
        Wasm_insist(not clone().is_nullptr(), "cannot dereference `nullptr`");
        return Reference<pointed_type>(*this);
    }

    /** Dereferencing a `const` pointer `PrimitiveExpr<T*>` yields a `ConstReference<T>`. */
    auto operator*() const requires dsl_primitive<pointed_type> {
        Wasm_insist(not clone().is_nullptr(), "cannot dereference `nullptr`");
        return ConstReference<pointed_type>(*this);
    }

    /** Dereferencing a pointer `PrimitiveExpr<T*>` yields a `Reference<T>`. */
    PrimitiveExpr<pointed_type>
    operator->()
    requires dsl_primitive<pointed_type>
    { return operator*(); } // implicitly convert from Reference<pointed_type>


    /*------------------------------------------------------------------------------------------------------------------
     * Pointer arithmetic
     *----------------------------------------------------------------------------------------------------------------*/

    ///> Adds a \p delta, in elements, to `this`.
    PrimitiveExpr operator+(PrimitiveExpr<offset_t> delta) {
        if constexpr (std::is_void_v<pointed_type>) {
            return PrimitiveExpr(addr_ + delta.make_unsigned(), offset_);
        } else {
            const uint32_t log_size = __builtin_ctzl(sizeof(pointed_type));
            return PrimitiveExpr(addr_ + (delta.make_unsigned() << log_size), offset_);
        }
    }

    ///> Adds a \p delta, in elements, to `this`.
    PrimitiveExpr operator+(offset_t delta) {
        if constexpr (std::is_void_v<pointed_type>) {
            offset_ += delta; // in bytes
        } else {
            const uint32_t log_size = __builtin_ctzl(sizeof(pointed_type));
            offset_ += delta << log_size; // in elements
        }
        return *this;
    }

    ///> Subtracts a \p delta, in elements, from `this`.
    PrimitiveExpr operator-(PrimitiveExpr<offset_t> delta) {
        if constexpr (std::is_void_v<pointed_type>) {
            return PrimitiveExpr(addr_ - delta.make_unsigned(), offset_);
        } else {
            const uint32_t log_size = __builtin_ctzl(sizeof(pointed_type));
            return PrimitiveExpr(addr_ - (delta.make_unsigned() << log_size), offset_);
        }
    }

    ///> Subtracts a \p delta, in elements, from `this`.
    PrimitiveExpr operator-(offset_t delta) {
        if constexpr (std::is_void_v<pointed_type>) {
            offset_ -= delta; // in bytes
        } else {
            const uint32_t log_size = __builtin_ctzl(sizeof(pointed_type));
            offset_ -= delta << log_size; // in elements
        }
        return *this;
    }

    ///> Computes the difference, in elements, between `this` and \p other.
    PrimitiveExpr<offset_t> operator-(PrimitiveExpr other) {
        if constexpr (std::is_void_v<pointed_type>) {
            PrimitiveExpr<offset_t> delta_addr = (this->addr_ - other.addr_).make_signed();
            offset_t delta_offset = this->offset_ - other.offset_;
            return (delta_offset ? (delta_addr + delta_offset) : delta_addr);
        } else {
            const int32_t log_size = __builtin_ctzl(sizeof(pointed_type));
            PrimitiveExpr<offset_t> delta_addr = (this->addr_ - other.addr_).make_signed() >> log_size;
            offset_t delta_offset = (this->offset_ - other.offset_) >> log_size;
            return (delta_offset ? (delta_addr + delta_offset) : delta_addr);

        }
    }

#define CMP_OP(SYMBOL) \
    /** Compares `this` to \p other by their addresses. */ \
    PrimitiveExpr<bool> operator SYMBOL(PrimitiveExpr other) { \
        return this->to<uint32_t>() SYMBOL other.to<uint32_t>(); \
    }
    CMP_OP(==)
    CMP_OP(!=)
    CMP_OP(<)
    CMP_OP(<=)
    CMP_OP(>)
    CMP_OP(>=)
#undef CMP_OP


    /*------------------------------------------------------------------------------------------------------------------
     * Load/Store operations
     *----------------------------------------------------------------------------------------------------------------*/

    private:
    PrimitiveExpr<pointed_type> load() requires dsl_primitive<pointed_type> {
        M_insist(bool(addr_), "address already moved or discarded");
        auto value = Module::Builder().makeLoad(
            /* bytes=  */ sizeof(pointed_type),
            /* signed= */ std::is_signed_v<pointed_type>,
            /* offset= */ offset_ >= 0 ? offset_ : 0,
            /* align=  */ alignof(pointed_type),
            /* ptr=    */ offset_ >= 0 ? addr_.expr() : (addr_ - uint32_t(-offset_)).expr(),
            /* type=   */ wasm_type<pointed_type>(),
            /* memory= */ Module::Get().memory_->name
        );
        return PrimitiveExpr<pointed_type>(value, addr_.referenced_bits());
    }

    ::wasm::Expression * store(PrimitiveExpr<pointed_type> value) requires dsl_primitive<pointed_type> {
        M_insist(bool(addr_), "address already moved or discarded");
        M_insist(bool(value), "value already moved or discarded");
        auto e = Module::Builder().makeStore(
            /* bytes=  */ sizeof(pointed_type),
            /* offset= */ offset_ >= 0 ? offset_ : 0,
            /* align=  */ alignof(pointed_type),
            /* ptr=    */ offset_ >= 0 ? addr_.expr() : (addr_ - uint32_t(-offset_)).expr(),
            /* value=  */ value.expr(),
            /* type=   */ wasm_type<pointed_type>(),
            /* memory= */ Module::Get().memory_->name
        );
        return e;
    }


    /*------------------------------------------------------------------------------------------------------------------
     * Printing
     *----------------------------------------------------------------------------------------------------------------*/

    public:
    friend std::ostream & operator<<(std::ostream &out, const PrimitiveExpr &P) {
        out << "PrimitiveExpr<" << typeid(pointed_type).name() << "*>: " << P.addr_ << " [" << P.offset_;
        return out;
    }

    void dump(std::ostream &out) const { out << *this << std::endl; }
    void dump() const { dump(std::cerr); }
};

namespace detail {

template<typename T>
struct ptr_helper;

template<>
struct ptr_helper<void>
{
    using type = PrimitiveExpr<void*>;
};

template<typename T>
struct ptr_helper<PrimitiveExpr<T>>
{
    using type = PrimitiveExpr<T*>;
};

}

/** Alias to easily declare `PrimitiveExpr` of pointer to primitive type. */
template<typename T>
using Ptr = typename detail::ptr_helper<T>::type;


/*======================================================================================================================
 * Expr
 *====================================================================================================================*/

/** An `Expr<T>` combines a `PrimitiveExpr<T>` value with a `PrimitiveExpr<bool>`, called NULL information, to implement
 * a value with *three-valued logic* (3VL).  `Expr<T>` provides the same operations as `PrimitiveExpr<T>`.  It delegates
 * operations to the underlying value and additionally combines the NULL information of the operand(s) into the new NULL
 * information of the result.  Particular exceptions are `operator and` and `operator or`, for which `Expr<T>`
 * implements 3VL according to [Kleene and Priest's
 * logic](https://en.wikipedia.org/wiki/Three-valued_logic#Kleene_and_Priest_logics). */
template<dsl_primitive T>
struct Expr<T>
{
    using type = T;

    /*----- Friends --------------------------------------------------------------------------------------------------*/
    template<typename> friend struct Expr; // to convert Expr<U> to Expr<T>
    template<typename, VariableKind, bool> friend class detail::variable_storage; // to use split_unsafe()

    private:
    ///> the referenced value expression
    PrimitiveExpr<T> value_;
    /** A boolean expression that evaluates to `true` at runtime iff this `Expr` is `NULL`.
     * If this `Expr` cannot be `NULL`, then `is_null_` evaluates to `false` at compile time, i.e. `not is_null_`.
     *
     * If `T` is a pointer type, then `is_null_` denotes whether the *pointed-to* value is `NULL`.  It does *not*
     * store whether the pointer is a `nullptr`! */
    PrimitiveExpr<bool> is_null_ = PrimitiveExpr<bool>();

    public:
    ///> *Implicitly* constructs an `Expr` from a \p value.
    Expr(PrimitiveExpr<T> value) : value_(value) {
        M_insist(bool(value_), "value must be present");
    }

    ///> Constructs an `Expr` from a \p value and NULL information \p is_null.
    Expr(PrimitiveExpr<T> value, PrimitiveExpr<bool> is_null)
        : value_(value)
        , is_null_(is_null)
    {
        M_insist(bool(value_), "value must be present");
    }

    ///> Constructs an `Expr` from a `std::pair` \p value of value and NULL info.
    explicit Expr(std::pair<PrimitiveExpr<T>, PrimitiveExpr<bool>> value)
        : Expr(value.first, value.second)
    { }

    /** Construct an `Expr<T>` from a primitive `T`. */
    explicit Expr(T value) : Expr(PrimitiveExpr<T>(value)) { }

    template<dsl_primitive U>
    requires same_signedness<T, U> and equally_floating<T, U>
    explicit Expr(U &&value)
        : Expr(T(std::forward<U>(value)))
    { }

    Expr(const Expr&) = delete;
    ///> Constructs a new `Expr<T>` by *moving* the underlying `value_` and `is_null_` of `other` to `this`.
    Expr(Expr &other) : Expr(other.split_unsafe()) { /* move, not copy */ }
    ///> Constructs a new `Expr<T>` by *moving* the underlying `value_` and `is_null_` of `other` to `this`.
    Expr(Expr &&other) : Expr(other.split_unsafe()) { }

    Expr & operator=(Expr&&) = delete;

    ~Expr() {
        M_insist(not bool(value_), "value must be used or explicitly discarded");
        M_insist(not bool(is_null_), "NULL flag must be used or explicitly discarded");
    }

    private:
    /** Splits this `Expr` into a `PrimitiveExpr<T>` with the value and a `PrimitiveExpr<bool>` with the `NULL`
     * information.  Then, *moves* these `PrimitiveExpr`s out of `this`.  Special care must be taken as the NULL
     * information may be unusable, i.e. missing AST. */
    std::pair<PrimitiveExpr<T>, PrimitiveExpr<bool>> split_unsafe() {
        M_insist(bool(value_), "`Expr` has already been moved");
        return { value_, is_null_ };
    }

    public:
    /** *Moves* the current `value_` out of `this`.  Requires (and insists) that `this` cannot be `NULL`. */
    PrimitiveExpr<T> insist_not_null() {
        M_insist(bool(value_), "`Expr` has already been moved");
        if (can_be_null())
            Wasm_insist(not is_null_, "must not be NULL");
        return value_;
    }

    /** Splits this `Expr` into a `PrimitiveExpr<T>` with the value and a `PrimitiveExpr<bool>` with the `NULL`
     * information.  Then, *moves* these `PrimitiveExpr`s out of `this`. */
    std::pair<PrimitiveExpr<T>, PrimitiveExpr<bool>> split() {
        M_insist(bool(value_), "`Expr` has already been moved");
        auto [value, is_null] = split_unsafe();
        if (is_null)
            return { value, is_null };
        else
            return { value, PrimitiveExpr<bool>(false) };
    }

    /** Returns a *deep copy* of `this`. */
    Expr clone() const {
        M_insist(bool(value_), "`Expr` has already been moved`");
        return Expr(
            /* value=   */ value_.clone(),
            /* is_null= */ is_null_ ? is_null_.clone() : PrimitiveExpr<bool>()
        );
    }

    /** Discards `this`. */
    void discard() {
        value_.discard();
        if (can_be_null())
            is_null_.discard();
    }


    /*------------------------------------------------------------------------------------------------------------------
     * methods related to NULL
     *----------------------------------------------------------------------------------------------------------------*/

    public:
    /** Returns `true` if `this` *may be* `NULL`, `false` otherwise. */
    bool can_be_null() const { return bool(is_null_); }

    /** Returns `true` if `this` is `NULL`, `false` otherwise. */
    PrimitiveExpr<bool> is_null() {
        value_.discard();
        if (can_be_null())
            return is_null_;
        else
            return PrimitiveExpr<bool>(false);
    }

    /** Returns `true` if `this` is `NOT NULL`, `false` otherwise. */
    PrimitiveExpr<bool> not_null() {
        value_.discard();
        if (can_be_null())
            return not is_null_;
        else
            return PrimitiveExpr<bool>(true);
    }

    /** Returns `true` if the value is `true` and `NOT NULL`.  Useful to use this `Expr<bool>` for conditional control
     * flow. */
    PrimitiveExpr<bool>
    is_true_and_not_null()
    requires boolean<T>
    {
        if (can_be_null())
            return value_ and not is_null_;
        else
            return value_;
    }

    /** Returns `true` if the value is `false` and `NOT NULL`.  Useful to use this `Expr<bool>` for conditional control
     * flow. */
    PrimitiveExpr<bool>
    is_false_and_not_null()
    requires boolean<T>
    {
        if (can_be_null())
            return not value_ and not is_null_;
        else
            return not value_;
    }


    /*------------------------------------------------------------------------------------------------------------------
     * Factory method for NULL
     *----------------------------------------------------------------------------------------------------------------*/

    public:
    /** Returns an `Expr<T>` that is `NULL`. */
    static Expr Null() { return Expr(PrimitiveExpr<T>(T()), PrimitiveExpr<bool>(true)); }


    /*------------------------------------------------------------------------------------------------------------------
     * Conversion operations
     *----------------------------------------------------------------------------------------------------------------*/

    public:
    /** *Implicitly* converts an `Expr<T>` to an `Expr<To>`.  Only applicable if `PrimitiveExpr<T>` is implicitly
     * convertible to `PrimitiveExpr<To>`. */
    template<dsl_primitive To>
    requires requires { static_cast<PrimitiveExpr<To>>(value_); }
    operator Expr<To>()
    { return Expr<To>(static_cast<PrimitiveExpr<To>>(value_), is_null_); }

    /** *Explicitly* converts an `Expr<T>` to an `Expr<To>`.  Only applicable if `PrimitiveExpr<T>` is explicitly
     * convertible to `PrimitiveExpr<To>` (via method `to<To>()`). */
    template<dsl_primitive To>
    requires requires { value_.template to<To>(); }
    Expr<To>
    to()
    { return Expr<To>(value_.template to<To>(), is_null_); }


    /*------------------------------------------------------------------------------------------------------------------
     * Unary operations
     *----------------------------------------------------------------------------------------------------------------*/

    public:
    /** List of supported unary operators on `PrimitiveExpr`. */
#define UNARY_LIST(X) \
    X(make_signed) \
    X(make_unsigned) \
    X(operator +) \
    X(operator -) \
    X(abs) \
    X(ceil) \
    X(floor) \
    X(sqrt) \
    X(operator ~) \
    X(clz) \
    X(ctz) \
    X(popcnt) \
    X(eqz) \
    X(operator not)

#define UNARY(OP) \
    auto OP() \
    requires requires (PrimitiveExpr<T> value) { value.OP(); } \
    { \
        using PrimExprT = decltype(value_.OP()); \
        using ExprT = expr_t<PrimExprT>; \
        return ExprT(value_.OP(), is_null_); \
    }
UNARY_LIST(UNARY)
#undef UNARY

    /*----- Hashing operations with special three-valued logic -------------------------------------------------------*/

    PrimitiveExpr<uint64_t> hash() { return Select(is_null_, PrimitiveExpr<uint64_t>(1UL << 63), value_.hash()); }


    /*------------------------------------------------------------------------------------------------------------------
     * Binary operations
     *----------------------------------------------------------------------------------------------------------------*/

    public:
#define BINARY(OP) \
    template<dsl_primitive U> \
    requires requires (PrimitiveExpr<T> lhs, PrimitiveExpr<U> rhs) { lhs.OP(rhs); } \
    auto OP(Expr<U> other) { \
        const unsigned idx = (other.can_be_null() << 1U) | this->can_be_null(); \
        auto result = this->value_.OP(other.value_); \
        using ReturnType = typename decltype(result)::type; \
        switch (idx) { \
            default: M_unreachable("invalid index"); \
            case 0b00: /* neither `this` nor `other` can be `NULL` */ \
                return Expr<ReturnType>(result); \
            case 0b01: /* `this` can be `NULL` */ \
                return Expr<ReturnType>(result, this->is_null_); \
            case 0b10: /* `other` can be `NULL` */ \
                return Expr<ReturnType>(result, other.is_null_); \
            case 0b11: /* both `this` and `other` can be `NULL` */ \
                return Expr<ReturnType>(result, this->is_null_ or other.is_null_); \
        } \
    }

    BINARY(operator +)
    BINARY(operator -)
    BINARY(operator *)
    BINARY(operator /)
    BINARY(operator %)
    BINARY(operator bitand)
    BINARY(operator bitor)
    BINARY(operator xor)
    BINARY(operator <<)
    BINARY(operator >>)
    BINARY(operator ==)
    BINARY(operator !=)
    BINARY(operator <)
    BINARY(operator <=)
    BINARY(operator >)
    BINARY(operator >=)
    BINARY(copy_sign)
    BINARY(min)
    BINARY(max)
    BINARY(rotl)
    BINARY(rotr)
#undef BINARY

    /*----- Logical operations with special three-valued logic -------------------------------------------------------*/

    /** Implements logical *and* according to 3VL of [Kleene and Priest's
     * logic](https://en.wikipedia.org/wiki/Three-valued_logic#Kleene_and_Priest_logics). */
    Expr<bool> operator and(Expr<bool> other)
    requires boolean<T>
    {
        const unsigned idx = (bool(other.is_null_) << 1U) | bool(this->is_null_);
        switch (idx) {
            default: M_unreachable("invalid index");

            case 0b00: { /* neither `this` nor `other` can be `NULL` */
                PrimitiveExpr<bool> result = this->value_ and other.value_;
                return Expr<bool>(result);
            }
            case 0b01: { /* `this` can be `NULL` */
                PrimitiveExpr<bool> result = this->value_ and other.value_.clone();
                PrimitiveExpr<bool> is_null =
                    this->is_null_ and  // `this` is NULL
                    other.value_;       // `other` does not dominate, i.e. is true
                return Expr<bool>(result, is_null);
            }
            case 0b10: { /* `other` can be `NULL` */
                PrimitiveExpr<bool> result = this->value_.clone() and other.value_;
                PrimitiveExpr<bool> is_null =
                    other.is_null_ and  // `other` is NULL
                    this->value_;       // `this` does not dominate, i.e. is true
                return Expr<bool>(result, is_null);
            }
            case 0b11: { /* both `this` and `other` can be `NULL` */
                auto this_is_null  = this->is_null_.clone();
                auto other_is_null = other.is_null_.clone();
                PrimitiveExpr<bool> result = this->value_.clone() and other.value_.clone();
                PrimitiveExpr<bool> is_null =
                    (this_is_null or other_is_null) and     // at least one is NULL
                    (this->value_ or this->is_null_) and    // `this` does not dominate, i.e. is not real false
                    (other.value_ or other.is_null_);       // `other` does not dominate, i.e. is not real false
                return Expr<bool>(result, is_null);
            }
        }
    }

    /** Implements logical *or* according to 3VL of [Kleene and Priest's
     * logic](https://en.wikipedia.org/wiki/Three-valued_logic#Kleene_and_Priest_logics). */
    Expr<bool> operator or(Expr<bool> other)
    requires boolean<T>
    {
        const unsigned idx = (bool(other.is_null_) << 1U) | bool(this->is_null_);
        switch (idx) {
            default: M_unreachable("invalid index");

            case 0b00: { /* neither `this` nor `other` can be `NULL` */
                PrimitiveExpr<bool> result = this->value_ or other.value_;
                return Expr<bool>(result);
            }
            case 0b01: { /* `this` can be `NULL` */
                PrimitiveExpr<bool> result = this->value_ or other.value_.clone();
                PrimitiveExpr<bool> is_null =
                    this->is_null_ and  // `this` is NULL
                    not other.value_;   // `other` does not dominate, i.e. is false
                return Expr<bool>(result, is_null);
            }
            case 0b10: { /* `other` can be `NULL` */
                PrimitiveExpr<bool> result = this->value_.clone() or other.value_;
                PrimitiveExpr<bool> is_null =
                    other.is_null_ and  // `other` is NULL
                    not this->value_;   // `this` does not dominate, i.e. is false
                return Expr<bool>(result, is_null);
            }
            case 0b11: { /* both `this` and `other` can be `NULL` */
                auto this_is_null  = this->is_null_.clone();
                auto other_is_null = other.is_null_.clone();
                PrimitiveExpr<bool> result = this->value_.clone() or other.value_.clone();
                PrimitiveExpr<bool> is_null =
                    (this_is_null or other_is_null) and         // at least one is NULL
                    (not this->value_ or this->is_null_) and    // `this` does not dominate, i.e. is not real true
                    (not other.value_ or other.is_null_);       // `other` does not dominate, i.e. is not real true
                return Expr<bool>(result, is_null);
            }
        }
    }


    /*------------------------------------------------------------------------------------------------------------------
     * Printing
     *----------------------------------------------------------------------------------------------------------------*/

    public:
    friend std::ostream & operator<<(std::ostream &out, const Expr &E) {
        out << "Expr<" << typeid(type).name() << ">: value_=" *E.value_ << ", is_null_=" << *E.is_null_;
        return out;
    }

    void dump(std::ostream &out) const { out << *this << std::endl; }
    void dump() const { dump(std::cerr); }
};

/** CTAD guide for `Expr` */
template<typename T>
Expr(PrimitiveExpr<T>, PrimitiveExpr<bool>) -> Expr<T>;

/*----- Forward binary operators on operands convertible to Expr<T> --------------------------------------------------*/
#define MAKE_BINARY(OP) \
    template<expr_convertible T, expr_convertible U> \
    requires (not primitive_convertible<T> or not primitive_convertible<U>) and \
    requires (expr_t<T> t, expr_t<U> u) { t.OP(u); } \
    auto OP(T &&t, U &&u) { \
        return expr_t<T>(std::forward<T>(t)).OP(expr_t<U>(std::forward<U>(u))); \
    }
BINARY_LIST(MAKE_BINARY)
#undef MAKE_BINARY

/*----- Short aliases for all `PrimitiveExpr` and `Expr` types. ------------------------------------------------------*/
#define USING(TYPE, NAME) \
    using NAME = PrimitiveExpr<TYPE>; \
    using _ ## NAME = Expr<TYPE>; \

    USING(bool,     Bool)
    USING(int8_t,   I8)
    USING(uint8_t,  U8)
    USING(int16_t,  I16)
    USING(uint16_t, U16)
    USING(int32_t,  I32)
    USING(uint32_t, U32)
    USING(int64_t,  I64)
    USING(uint64_t, U64)
    USING(float,    Float)
    USING(double,   Double)
    USING(char,     Char) ///< this is neither signed nor unsigned char (see
                          ///< https://en.cppreference.com/w/cpp/language/types, Character types)
#undef USING


/*======================================================================================================================
 * Variable
 *====================================================================================================================*/

namespace detail {

/** Allocates a fresh local variable of type \tparam T in the currently active function's stack and returns the
 * variable's `::wasm::Index`. */
template<dsl_primitive T>
::wasm::Index allocate_local()
{
    ::wasm::Function &fn = Module::Function();
    const ::wasm::Index index = fn.getNumParams() + fn.vars.size();
    const ::wasm::Type type = wasm_type<T>();
    fn.vars.emplace_back(type); // allocate new local variable
    M_insist(fn.isVar(index));
    M_insist(fn.getLocalType(index) == type);
    return index;
}

/** Helper class to select the appropriate storage for a `Variable`.  Local variables are allocated on the currently
 * active function's stack whereas global variables are allocated globally.  Local variables of primitive type
 * can have an additional `NULL` information. */
template<typename T, VariableKind Kind, bool CanBeNull>
class variable_storage;

/** Specialization for local variables of arithmetic type that *cannot* be `NULL`. */
template<dsl_primitive T, VariableKind Kind>
requires arithmetic<T> or (boolean<T> and Kind == VariableKind::Param)
class variable_storage<T, Kind, /* CanBeNull= */ false>
{
    using type = T;

    template<typename, VariableKind, bool>
    friend class variable_storage; // to enable use in other `variable_storage`
    friend struct Variable<T, Kind, false>; // to be usable by the respective Variable

    ::wasm::Index index_; ///< the index of the local variable
    ::wasm::Type type_; ///< the type of the local variable

    /** Default-construct. */
    variable_storage() : index_(allocate_local<T>()) , type_(wasm_type<T>()) { }

    variable_storage(const variable_storage&) = delete;
    variable_storage(variable_storage&&) = default;
    variable_storage & operator=(variable_storage&&) = default;

    /** Construct from `::wasm::Index` of already allocated local. */
    variable_storage(::wasm::Index idx, tag<int>) : index_(idx), type_(wasm_type<T>()) {
#ifndef NDEBUG
        ::wasm::Function &fn = Module::Function();
        M_insist(fn.isParam(index_));
        M_insist(fn.getLocalType(index_) == type_);
#endif
    }

    /** Construct from value. */
    explicit variable_storage(T value) : variable_storage() { operator=(PrimitiveExpr<T>(value)); }

    /** Construct from value. */
    template<primitive_convertible U>
    requires requires (U &&u) { PrimitiveExpr<T>(primitive_expr_t<U>(std::forward<U>(u))); }
    explicit variable_storage(U &&value) : variable_storage() { operator=(std::forward<U>(value)); }

    /** Assign value. */
    template<primitive_convertible U>
    requires requires (U &&u) { PrimitiveExpr<T>(primitive_expr_t<U>(std::forward<U>(u))); }
    void operator=(U &&u) {
        PrimitiveExpr<T> value(primitive_expr_t<U>(std::forward<U>(u)));
        Module::Block().list.push_back(Module::Builder().makeLocalSet(index_, value.expr()));
    }

    /** Retrieve value. */
    operator PrimitiveExpr<T>() const { return PrimitiveExpr<T>(Module::Builder().makeLocalGet(index_, type_)); }
};

/** Specialization for local variables of boolean type that *cannot* be `NULL`. */
template<>
class variable_storage<bool, VariableKind::Local, /* CanBeNull= */ false>
{
    using type = bool;

    template<typename, VariableKind, bool>
    friend class variable_storage; // to enable use in other `variable_storage`
    friend struct Variable<bool, VariableKind::Local, false>; // to be usable by the respective Variable

    std::shared_ptr<LocalBit> value_; ///< stores the boolean value in a single bit

    /** Default-construct. */
    variable_storage(); // impl delayed because `LocalBit` defined later

    variable_storage(const variable_storage&) = delete;
    variable_storage(variable_storage&&) = default;
    variable_storage & operator=(variable_storage&&) = default;

    /** Construct from value. */
    explicit variable_storage(bool value) : variable_storage() { operator=(PrimitiveExpr<bool>(value)); }

    /** Construct from value. */
    template<primitive_convertible U>
    requires requires (U &&u) { PrimitiveExpr<bool>(primitive_expr_t<U>(std::forward<U>(u))); }
    explicit variable_storage(U &&value) : variable_storage() { operator=(std::forward<U>(value)); }

    /** Assign value. */
    template<primitive_convertible U>
    requires requires (U &&u) { PrimitiveExpr<bool>(primitive_expr_t<U>(std::forward<U>(u))); }
    void operator=(U &&u); // impl delayed because `LocalBit` defined later

    /** Retrieve value. */
    operator PrimitiveExpr<bool>() const; // impl delayed because `LocalBit` defined later
};

/** Specialization for local variables of primitive type (arithmetic and boolean) that *can* be `NULL`. */
template<dsl_primitive T>
class variable_storage<T, VariableKind::Local, /* CanBeNull= */ true>
{
    using type = T;

    friend struct Variable<T, VariableKind::Local, true>; // to be usable by the respective Variable

    variable_storage<T, VariableKind::Local, false> value_;
    variable_storage<bool, VariableKind::Local, false> is_null_;

    /** Default-construct. */
    variable_storage() = default;

    /** Construct from value. */
    explicit variable_storage(T value) : variable_storage() { operator=(Expr<T>(value)); }

    /** Construct from value. */
    template<expr_convertible U>
    requires requires (U &&u) { Expr<T>(expr_t<U>(std::forward<U>(u))); }
    explicit variable_storage(U &&value) : variable_storage() { operator=(std::forward<U>(value)); }

    /** Assign value. */
    template<expr_convertible U>
    requires requires (U &&u) { Expr<T>(expr_t<U>(std::forward<U>(u))); }
    void operator=(U &&u) {
        Expr<T> _value(expr_t<U>(std::forward<U>(u)));
        auto [value, is_null] = _value.split_unsafe();
        this->value_ = value;
        this->is_null_ = bool(is_null) ? is_null : PrimitiveExpr<bool>(false);
    }

    /** Retrieve value. */
    operator Expr<T>() const { return Expr<T>(PrimitiveExpr<T>(value_), PrimitiveExpr<bool>(is_null_)); }
};

/** Specialization for local variables of pointer to primitive type.  Pointers *cannot* be `NULL`. */
template<dsl_pointer_to_primitive T, VariableKind Kind>
requires (Kind != VariableKind::Global)
class variable_storage<T, Kind, /* CanBeNull= */ false>
{
    using type = T;
    using pointed_type = std::remove_pointer_t<T>;

    friend struct Variable<T, Kind, false>; // to be usable by the respective Variable

    ///> the address
    variable_storage<uint32_t, Kind, false> addr_;

    /** Default-construct. */
    variable_storage() = default;

    /** Construct from `::wasm::Index` of already allocated local. */
    explicit variable_storage(::wasm::Index idx, tag<int> tag) : addr_(idx, tag) { }

    /** Construct from pointer. */
    template<typename U>
    requires requires (U &&u) { PrimitiveExpr<T>(primitive_expr_t<U>(std::forward<U>(u))); }
    explicit variable_storage(U &&u) : variable_storage() { operator=(std::forward<U>(u)); }

    /** Assign pointer. */
    template<typename U>
    requires requires (U &&u) { PrimitiveExpr<T>(primitive_expr_t<U>(std::forward<U>(u))); }
    void operator=(U &&u) { addr_ = PrimitiveExpr<T>(primitive_expr_t<U>(std::forward<U>(u))).template to<uint32_t>(); }

    /** Retrieve pointer. */
    operator PrimitiveExpr<T>() const { return PrimitiveExpr<uint32_t>(addr_).template to<T>(); }
};

/** Specialization for global variables of primitive or pointer to primitive type \tparam T.  Global variables must be
 * of primitive or pointer to primitive type and *cannot* be `NULL`. */
template<typename T>
requires dsl_primitive<T> or dsl_pointer_to_primitive<T>
class variable_storage<T, VariableKind::Global, /* CanBeNull= */ false>
{
    using type = T;

    friend struct Variable<T, VariableKind::Global, false>;

    ::wasm::Name name_; ///< the global's unique name
    ::wasm::Type type_; ///< the type of the global variable

    /** Default construct. */
    variable_storage()
    requires dsl_primitive<T>
        : variable_storage(T())
    { }

    variable_storage(const variable_storage&) = delete;
    variable_storage(variable_storage&&) = default;
    variable_storage & operator=(variable_storage&&) = default;

    /** Construct with optional initial value. */
    template<dsl_primitive U>
    requires requires (U u) { Module::Get().emit_global<T>(name_, u); }
    explicit variable_storage(U init = U())
    requires dsl_primitive<T>
        : name_(Module::Unique_Global_Name())
        , type_(wasm_type<T>())
    {
        Module::Get().emit_global<T>(name_, init);
    }
    /** Construct with optional initial value. */
    explicit variable_storage(uint32_t init = 0)
    requires dsl_pointer_to_primitive<T>
        : name_(Module::Unique_Global_Name())
        , type_(wasm_type<T>())
    {
        Module::Get().emit_global<T>(name_, init);
    }

    /** Sets the initial value. */
    template<dsl_primitive U>
    requires requires (U u) { make_literal<T>(u); }
    void init(U init)
    requires dsl_primitive<T> {
        Module::Get().module_.getGlobal(name_)->init = Module::Builder().makeConst(make_literal<T>(init));
    }
    /** Sets the initial value. */
    void init(uint32_t init)
    requires dsl_pointer_to_primitive<T> {
        Module::Get().module_.getGlobal(name_)->init = Module::Builder().makeConst(::wasm::Literal(init));
    }

    /** Assign value. */
    template<typename U>
    requires requires (U &&u) { PrimitiveExpr<T>(primitive_expr_t<U>(std::forward<U>(u))); }
    void operator=(U &&u) {
        PrimitiveExpr<T> value(primitive_expr_t<U>(std::forward<U>(u)));
        Module::Block().list.push_back(Module::Builder().makeGlobalSet(name_, value.expr()));
    }

    /** Retrieve value. */
    operator PrimitiveExpr<T>() const { return PrimitiveExpr<T>(Module::Builder().makeGlobalGet(name_, type_)); }
};

}

template<typename T, VariableKind Kind, bool CanBeNull>
requires (dsl_primitive<T> or dsl_pointer_to_primitive<T>) and  // T is DSL primitive or DSL pointer to primitive
         (not (Kind == VariableKind::Global and CanBeNull)) and // globals cannot be NULL
         (not (dsl_pointer_to_primitive<T> and CanBeNull))      // pointers cannot be NULL
struct Variable<T, Kind, CanBeNull>
{
    using type = T;
    template<typename X>
    using dependent_expr_t = conditional_one_t<CanBeNull, expr_t, primitive_expr_t, X>;
    using dependent_expr_type = dependent_expr_t<T>;

    private:
    ///> the type of storage for this `Variable`
    using storage_type = detail::variable_storage<T, Kind, CanBeNull>;
    ///> storage of this `Variable`
    storage_type storage_;

    public:
    /** Default-constructs a new `Variable`. */
    Variable() requires requires { storage_type(); } { }

    /** Constructs a new `Variable` and initializes it with \p t. */
    template<typename U>
    requires requires (U &&value) { storage_type(std::forward<U>(value)); }
    explicit Variable(U &&value) : storage_(std::forward<U>(value)) { }

    protected:
    /** Constructs a `Variable` instance from an already allocated local with the given index \p idx.  Used by
     * `Parameter` to create `Variable` instances for function parameters. */
    Variable(::wasm::Index idx, tag<int> tag)
    requires (Kind == VariableKind::Param)
        : storage_(idx, tag)
    { }

    public:
    /** Check whether this `Variable` can be assigned to `NULL`, i.e. it has a NULL bit to store this information.
     * This is a compile-time information. */
    constexpr bool has_null_bit() const { return CanBeNull; }
    /** Check whether the value of this `Variable` can be `NULL`.  This is a runtime-time information. */
    bool can_be_null() const {
        if constexpr (CanBeNull)
            return dependent_expr_type(*this).can_be_null();
        else
            return false;
    }

    /** Obtain a `Variable<T>`s value as a `PrimitiveExpr<T>` or `Expr<T>`, depending on `CanBeNull`.  Although a
     * `Variable`'s value can also be obtained through implicit conversion (see below), some C/C++ constructs fail to do
     * so (e.g. arguments to calls) and it is therefore more convenient to call `val()`. */
    dependent_expr_type val() const { return dependent_expr_type(storage_); }

    /** Obtain a `Variable<T>`s value as a `PrimitiveExpr<T>` or `Expr<T>`, depending on `CanBeNull`.  This implicit
     * conversion enables using a `Variable` much like a `PrimitiveExpr` or `Expr`, respectively. */
    operator dependent_expr_type() const { return dependent_expr_type(storage_); }

    template<typename U>
    requires requires (dependent_expr_type v) { dependent_expr_t<U>(v); }
    operator dependent_expr_t<U>() const { return dependent_expr_t<U>(dependent_expr_type(*this)); }

    template<typename U>
    requires requires (dependent_expr_type v) { v.template to<U>(); }
    dependent_expr_t<U> to() const { return dependent_expr_type(*this).template to<U>(); }

    template<typename U>
    requires requires (U init) { storage_.init(init); }
    void init(U init) { storage_.init(init); }

    template<typename U>
    requires requires (U &&value) { storage_ = std::forward<U>(value); }
    Variable & operator=(U &&value) { storage_ = std::forward<U>(value); return *this; }


    /*------------------------------------------------------------------------------------------------------------------
     * Forward operators on Variable<T>
     *----------------------------------------------------------------------------------------------------------------*/

    /*----- Unary operators ------------------------------------------------------------------------------------------*/
#define UNARY(OP) \
    auto OP() const \
    requires requires (dependent_expr_type e) { e.OP(); } \
    { return dependent_expr_type(*this).OP(); }

    UNARY_LIST(UNARY)
    UNARY(hash)                     // from PrimitiveExpr and Expr
    UNARY(is_nullptr)               // from PrimitiveExpr for pointers
    UNARY(is_null)                  // from Expr
    UNARY(not_null)                 // from Expr
    UNARY(is_true_and_not_null)     // from Expr
    UNARY(is_false_and_not_null)    // from Expr
#undef UNARY

    /*----- Assignment operators -------------------------------------------------------------------------------------*/
#define ASSIGNOP_LIST(X) \
    X(+) \
    X(-) \
    X(*) \
    X(/) \
    X(%) \
    X(&) \
    X(|) \
    X(^) \
    X(<<) \
    X(>>)

#define ASSIGNOP(SYMBOL) \
    template<typename U> \
    requires requires { typename dependent_expr_t<U>; } and \
             requires (U &&u) { dependent_expr_t<U>(std::forward<U>(u)); } and \
             requires (dependent_expr_type var_value, dependent_expr_t<U> other_value) \
                      { var_value SYMBOL other_value; } and \
             requires (Variable var, decltype(std::declval<dependent_expr_type>() SYMBOL std::declval<dependent_expr_t<U>>()) value) \
                      { var = value; } \
    Variable & operator SYMBOL##= (U &&u) { \
        dependent_expr_t<U> value(std::forward<U>(u)); \
        this->operator=(dependent_expr_type(*this) SYMBOL value); \
        return *this; \
    }
ASSIGNOP_LIST(ASSIGNOP)
#undef ASSIGNOP


    /*------------------------------------------------------------------------------------------------------------------
     * Special operations for pointers
     *----------------------------------------------------------------------------------------------------------------*/

    auto operator*()
    requires dsl_pointer_to_primitive<T> { return Reference<std::remove_pointer_t<T>>(PrimitiveExpr<T>(*this)); }

    auto operator*() const
    requires dsl_pointer_to_primitive<T> { return ConstReference<std::remove_pointer_t<T>>(PrimitiveExpr<T>(*this)); }
};

namespace detail {

/** Deduces a suitable specialization of `Variable` for the given type \tparam T. */
template<typename T>
struct var_helper;

template<typename T>
struct var_helper<PrimitiveExpr<T>>
{ using type = Variable<T, VariableKind::Local, /* CanBeNull= */ false>; };

template<typename T>
struct var_helper<Expr<T>>
{ using type = Variable<T, VariableKind::Local, /* CanBeNull= */ true>; };

/** Deduces a suitable specialization of `Variable` *that can be NULL* for the given type \tparam T. */
template<typename T>
struct _var_helper;

template<typename T>
struct _var_helper<PrimitiveExpr<T>>
{ using type = Variable<T, VariableKind::Local, /* CanBeNull= */ true>; };

template<typename T>
struct _var_helper<Expr<T>>
{ using type = Variable<T, VariableKind::Local, /* CanBeNull= */ true>; };

/** Deduces a suitable specialization of `Variable` for global variables of the given type \tparam T. */
template<typename T>
struct global_helper;

template<typename T>
struct global_helper<PrimitiveExpr<T>>
{ using type = Variable<T, VariableKind::Global, /* CanBeNull= */ false>; };

}

/** Local variable. Can be `NULL` if \tparam T can be `NULL`. */
template<typename T>
requires requires { typename detail::var_helper<T>::type; }
using Var = typename detail::var_helper<T>::type;

/** Local variable that *can always* be `NULL`. */
template<typename T>
requires requires { typename detail::_var_helper<T>::type; }
using _Var = typename detail::_var_helper<T>::type;

/** Global variable.  Cannot be `NULL`. */
template<typename T>
requires requires { typename detail::global_helper<T>::type; }
using Global = typename detail::global_helper<T>::type;


/*======================================================================================================================
 * Parameter
 *====================================================================================================================*/

/** A type to access function parameters.  Function parameters are like local variables, but they need not be explicitly
 * allocated on the stack but are implicitly allocated by the function's signature.  Parameters are indexed in the order
 * they occur in the function signature. */
template<typename T>
struct Parameter : Variable<T, VariableKind::Param, /* CanBeNull= */ false>
{
    template<typename>
    friend struct Function; // to enable `Function` to create `Parameter` instances through private c'tor

    using base_type = Variable<T, VariableKind::Param, /* CanBeNull= */ false>;
    using base_type::operator=;
    using dependent_expr_type = typename base_type::dependent_expr_type;
    using base_type::operator dependent_expr_type;

    private:
    /** Create a `Parameter<T>` for the existing parameter local of given `index`.  Parameters can only be created by
     * `Function::parameter<I>()`. */
    Parameter(unsigned index)
        : base_type(::wasm::Index(index), tag<int>{})
    {
        ::wasm::Function &fn = Module::Function();
        M_insist(index < fn.getNumLocals(), "index out of bounds");
        M_insist(fn.isParam(index), "not a parameter");
        M_insist(fn.getLocalType(index) == wasm_type<T>(), "type mismatch");
    }
};


/*======================================================================================================================
 * Pointer and References
 *====================================================================================================================*/

namespace detail {

template<dsl_primitive T, bool IsConst>
struct the_reference
{
    friend struct PrimitiveExpr<T*>; // to construct a reference to the pointed-to memory
    friend struct Variable<T*, VariableKind::Local, false>; // to construct a reference to the pointed-to memory
    friend struct Variable<T*, VariableKind::Global, false>; // to construct a reference to the pointed-to memory
    friend struct Variable<T*, VariableKind::Param, false>; // to construct a reference to the pointed-to memory

    static constexpr bool is_const = IsConst;

    private:
    PrimitiveExpr<T*> ptr_;

    private:
    explicit the_reference(PrimitiveExpr<T*> ptr)
        : ptr_(ptr)
    {
        M_insist(bool(ptr_), "must not be moved or discarded");
    }

    public:
    template<typename U>
    requires (not is_const) and requires (U &&u) { PrimitiveExpr<T>(primitive_expr_t<U>(std::forward<U>(u))); }
    void operator=(U &&u) {
        PrimitiveExpr<T> value(primitive_expr_t<U>(std::forward<U>(u)));
        Module::Block().list.push_back(ptr_.store(value));
    }

    ///> implicit loading of the referenced value
    operator PrimitiveExpr<T>() { return ptr_.load(); }

#define ASSIGNOP(SYMBOL) \
    template<typename U> \
    requires requires (the_reference ref, U &&u) { ref SYMBOL std::forward<U>(u); } and \
             requires (the_reference ref, decltype(std::declval<PrimitiveExpr<T>>() SYMBOL std::declval<U>()) value) \
                      { ref = value; } \
    void operator SYMBOL##= (U &&u) { \
        this->operator=(the_reference(ptr_.clone()) SYMBOL std::forward<U>(u)); \
    }
ASSIGNOP_LIST(ASSIGNOP)
#undef ASSIGNOP
};

}


/*======================================================================================================================
 * LocalBitmap and LocalBit
 *====================================================================================================================*/

struct LocalBitmap
{
    friend struct Module;

    Var<U64> u64;
    uint64_t bitmask = uint64_t(-1UL);

    private:
    LocalBitmap() = default;
    LocalBitmap(const LocalBitmap&) = delete;
};

/**
 * A bit that is managed by the current function's stack.
 *
 * 0 ⇔ false ⇔ NOT NULL
 * 1 ⇔ true  ⇔ NULL
 */
struct LocalBit
{
    friend void swap(LocalBit &first, LocalBit &second) {
        using std::swap;
        swap(first.bitmap_,     second.bitmap_);
        swap(first.bit_offset_, second.bit_offset_);
    }

    friend struct Module; // to construct LocalBit

    private:
    LocalBitmap *bitmap_ = nullptr; ///< the bitmap in which the *single* bit is contained
    uint8_t bit_offset_; ///< the offset of the *single* bit

    LocalBit() = default;
    /** Creates a bit with storage allocated in \p bitmap. */
    LocalBit(LocalBitmap &bitmap, uint8_t bit_offset) : bitmap_(&bitmap), bit_offset_(bit_offset) {
        M_insist(bit_offset_ < CHAR_BIT * sizeof(uint64_t), "offset out of bounds");
    }

    public:
    LocalBit(const LocalBit&) = delete;
    LocalBit(LocalBit &&other) : LocalBit() { swap(*this, other); }
    ~LocalBit();

    LocalBit & operator=(LocalBit &&other) { swap(*this, other); return *this; }

    ///> Returns the offset of the bit within a `LocalBitmap`.
    uint64_t offset() const { return bit_offset_; }
    ///> Returns a mask with a single bit set at offset `offset()`.
    uint64_t mask() const { return 1UL << bit_offset_; }

    public:
    /** Returns the boolean expression that evaluates to `true` if the bit is set, `false` otherwise. */
    PrimitiveExpr<bool> is_set() const;
    /** Sets this bit. */
    void set();
    /** Clears this bit. */
    void clear();
    /** Sets this bit to the boolean value of \p value. */
    void set(PrimitiveExpr<bool> value);
    /** Sets `this` bit to the value of bit `other`.  Cleverly computes required shift width at compile time to use only
     * a single shift operation. */
    LocalBit & operator=(const LocalBit &other);
    /** Converts this `LocalBit` to `PrimitiveExpr<bool>`, which is `true` iff this `LocalBit` is set. */
    operator PrimitiveExpr<bool>() const { return is_set(); }
};


/*======================================================================================================================
 * Control flow
 *====================================================================================================================*/

/*----- Return unsafe, i.e. without static type checking -------------------------------------------------------------*/

inline void RETURN_UNSAFE() { Module::Get().emit_return(); }

template<primitive_convertible T>
inline void RETURN_UNSAFE(T &&t) { Module::Get().emit_return(expr_t<T>(std::forward<T>(t))); }

template<expr_convertible T>
requires (not primitive_convertible<T>)
inline void RETURN_UNSAFE(T &&t) { Module::Get().emit_return(expr_t<T>(std::forward<T>(t))); }

/*----- BREAK --------------------------------------------------------------------------------------------------------*/

inline void BREAK(std::size_t level = 1) { Module::Get().emit_break(level); }
template<primitive_convertible C>
requires requires (C &&c) { PrimitiveExpr<bool>(std::forward<C>(c)); }
inline void BREAK(C &&_cond, std::size_t level = 1)
{
    PrimitiveExpr<bool> cond(std::forward<C>(_cond));
    Module::Get().emit_break(cond, level);
}

/*----- CONTINUE -----------------------------------------------------------------------------------------------------*/

inline void CONTINUE(std::size_t level = 1) { Module::Get().emit_continue(level); }
template<primitive_convertible C>
requires requires (C &&c) { PrimitiveExpr<bool>(std::forward<C>(c)); }
inline void CONTINUE(C &&_cond, std::size_t level = 1)
{
    PrimitiveExpr<bool> cond(std::forward<C>(_cond));
    Module::Get().emit_continue(cond, level);
}

/*----- Select -------------------------------------------------------------------------------------------------------*/

template<primitive_convertible C, primitive_convertible T, primitive_convertible U>
requires have_common_type<typename primitive_expr_t<T>::type, typename primitive_expr_t<U>::type> and
requires (C &&c) { PrimitiveExpr<bool>(std::forward<C>(c)); }
inline auto Select(C &&_cond, T &&_tru, U &&_fals)
{
    PrimitiveExpr<bool> cond(std::forward<C>(_cond));
    primitive_expr_t<T> tru(std::forward<T>(_tru));
    primitive_expr_t<U> fals(std::forward<U>(_fals));

    using To = common_type_t<typename decltype(tru)::type, typename decltype(fals)::type>;

    return Module::Get().emit_select<To>(cond, tru.template to<To>(), fals.template to<To>());
}

template<primitive_convertible C, expr_convertible T, expr_convertible U>
requires (not primitive_convertible<T> or not primitive_convertible<U>) and
         have_common_type<typename expr_t<T>::type, typename expr_t<U>::type> and
requires (C &&c) { PrimitiveExpr<bool>(std::forward<C>(c)); }
inline auto Select(C &&_cond, T &&_tru, U &&_fals)
{
    PrimitiveExpr<bool> cond(std::forward<C>(_cond));
    expr_t<T> tru(std::forward<T>(_tru));
    expr_t<U> fals(std::forward<U>(_fals));

    using To = common_type_t<typename decltype(tru)::type, typename decltype(fals)::type>;

    return Module::Get().emit_select<To>(cond, tru.template to<To>(), fals.template to<To>());
}


/*----- If -----------------------------------------------------------------------------------------------------------*/

struct If
{
    using continuation_t = std::function<void(void)>;

    private:
    PrimitiveExpr<bool> cond_;
    std::string name_;

    public:
    continuation_t Then, Else;

    template<primitive_convertible C>
    requires requires (C &&c) { PrimitiveExpr<bool>(std::forward<C>(c)); }
    explicit If(C &&cond)
        : cond_(std::forward<C>(cond))
        , name_(Module::Unique_If_Name())
    { }

    If(const If&) = delete;
    If(If&&) = delete;

    ~If();
};

/*----- Loop ---------------------------------------------------------------------------------------------------------*/

/** Implements a loop which iterates exactly once unless explicitly `continue`-ed.  The loop may be exited by
 * explicitly `break`-ing out of it. */
struct Loop
{
    friend void swap(Loop &first, Loop &second) {
        using std::swap;
        swap(first.body_, second.body_);
        swap(first.loop_, second.loop_);
    }

    private:
    Block body_; ///< the loop body
    ::wasm::Loop *loop_ = nullptr; ///< the Binaryen loop

    private:
    /** Convenience c'tor accessible via tag-dispatching.  Expects an already unique \p name. */
    Loop(std::string name, tag<int>)
        : body_(name + ".body", false)
        , loop_(M_notnull(Module::Builder().makeLoop(name, &body_.get())))
    {
        Module::Get().push_branch_targets(
            /* brk=     */ body_.get().name,
            /* continu= */ loop_->name
        );
    }

    public:
    explicit Loop(std::string name) : Loop(Module::Unique_Loop_Name(name), tag<int>{}) { }
    explicit Loop(const char *name) : Loop(std::string(name)) { }

    Loop(const Loop&) = delete;
    Loop(Loop &&other) { swap(*this, other); }

    ~Loop() {
        if (loop_) {
            Module::Get().pop_branch_targets();
            Module::Block().list.push_back(loop_);
        }
    }

    Loop & operator=(Loop &&other) { swap(*this, other); return *this; }

    std::string name() const { return loop_->name.toString(); }

    Block & body() { return body_; }
    const Block & body() const { return body_; }
};

struct DoWhile : Loop
{
    template<primitive_convertible C>
    requires requires (C &&c) { PrimitiveExpr<bool>(std::forward<C>(c)); }
    DoWhile(std::string name, C &&_cond)
        : Loop(name)
    {
        PrimitiveExpr<bool> cond(std::forward<C>(_cond));

        /*----- Update condition in branch targets. -----*/
        auto branch_targets = Module::Get().pop_branch_targets();
        Module::Get().push_branch_targets(branch_targets.brk, branch_targets.continu, cond);
    }

    template<primitive_convertible C>
    requires requires (C &&c) { PrimitiveExpr<bool>(std::forward<C>(c)); }
    DoWhile(const char *name, C &&cond) : DoWhile(std::string(name), cond) { }

    DoWhile(const DoWhile&) = delete;
    DoWhile(DoWhile&&) = default;

    ~DoWhile();
};

struct While
{
    private:
    PrimitiveExpr<bool> cond_;
    std::unique_ptr<DoWhile> do_while_;

    public:
    While(std::string name, PrimitiveExpr<bool> cond)
        : cond_(cond.clone())
        , do_while_(std::make_unique<DoWhile>(name + ".do-while", cond))
    { }

    template<primitive_convertible C>
    requires requires (C &&c) { PrimitiveExpr<bool>(std::forward<C>(c)); }
    While(std::string name, C &&cond) : While(name, PrimitiveExpr<bool>(std::forward<C>(cond))) { }

    template<primitive_convertible C>
    requires requires (C &&c) { PrimitiveExpr<bool>(std::forward<C>(c)); }
    While(const char *name, C &&cond) : While(std::string(name), cond) { }

    While(const While&) = delete;
    While(While&&) = default;

    ~While();

    Block & body() { return do_while_->body(); }
    const Block & body() const { return do_while_->body(); }
};


/*======================================================================================================================
 * Allocator
 *====================================================================================================================*/

struct Allocator
{
    public:
    virtual ~Allocator() { }

    public:
    /** Pre-allocates memory for \p bytes consecutive bytes with alignment requirement \p align and returns a pointer
     * to the beginning of this memory. */
    virtual Ptr<void> pre_allocate(uint32_t bytes, uint32_t align = 1) = 0;
    /** Allocates memory for \p bytes consecutive bytes with alignment requirement \p align and returns a pointer to the
     * beginning of this memory. */
    virtual Var<Ptr<void>> allocate(U32 bytes, uint32_t align = 1) = 0;
    /** Deallocates the `bytes` consecutive bytes of allocated memory at address `ptr`. */
    virtual void deallocate(Ptr<void> ptr, U32 bytes) = 0;

    /** Performs the actual pre-allocations.  Must be called exactly **once** **after** the last pre-allocation was
     * requested. */
    virtual void perform_pre_allocations() = 0;

    Var<Ptr<void>> allocate(uint32_t bytes, uint32_t align = 1) { return allocate(U32(bytes), align); }
    void deallocate(Ptr<void> ptr, uint32_t bytes) { return deallocate(ptr, U32(bytes)); }

    /** Pre-allocates memory for exactly one value of type \tparam T and returns a pointer to this memory. */
    template<dsl_primitive T>
    Ptr<PrimitiveExpr<T>> pre_malloc() { return pre_malloc<T>(1U); }
    /** Allocates memory for exactly one value of type \tparam T and returns a pointer to this memory. */
    template<dsl_primitive T>
    Var<Ptr<PrimitiveExpr<T>>> malloc() { return malloc<T>(1U); }

    /** Pre-allocates memory for an array of \p count consecutive values of type \tparam T and returns a pointer to this
     * memory. */
    template<dsl_primitive T>
    Ptr<PrimitiveExpr<T>> pre_malloc(uint32_t count) {
        return pre_allocate(sizeof(T) * count, alignof(T)).template to<T*>();
    }
    /** Allocates memory for an array of \p count consecutive values of type \tparam T and returns a pointer to this
     * memory. */
    template<dsl_primitive T, typename U>
    requires requires (U &&u) { U32(std::forward<U>(u)); }
    Var<Ptr<PrimitiveExpr<T>>> malloc(U &&count) {
        Var<Ptr<PrimitiveExpr<T>>> ptr(allocate(
            U32(static_cast<uint32_t>(sizeof(T)) * std::forward<U>(count)),
            alignof(T)
        ).template to<T*>());
        return ptr;
    }

    /** Frees exactly one value of type \tparam T of allocated memory pointed by \p ptr. */
    template<typename T>
    requires requires { typename primitive_expr_t<T>; } and
             requires (T &&t) { primitive_expr_t<T>(std::forward<T>(t)).template to<void*>(); }
    void free(T &&ptr) { free(std::forward<T>(ptr), 1U); }

    /** Frees \p count consecutive values of type \tparam T of allocated memory pointed by \p ptr. */
    template<typename T, typename U>
    requires requires (U &&u) { U32(std::forward<U>(u)); } and
             requires { typename primitive_expr_t<T>; } and
             requires (T &&t) { primitive_expr_t<T>(std::forward<T>(t)).template to<void*>(); }
    void free(T &&ptr, U &&count) {
        primitive_expr_t<T> _ptr(std::forward<T>(ptr));
        using pointed_type = typename decltype(_ptr)::pointed_type;
        deallocate(
            _ptr.template to<void*>(),
            U32(uint32_t(sizeof(pointed_type)) * std::forward<U>(count))
        );
    }
};


/*######################################################################################################################
 * DELAYED DEFINITIONS
 *####################################################################################################################*/

/*======================================================================================================================
 * Module
 *====================================================================================================================*/

inline void Module::create_local_bitmap_stack()
{
    local_bitmaps_stack_.emplace_back();
}

inline void Module::dispose_local_bitmap_stack()
{
    auto &local_bitmaps = local_bitmaps_stack_.back();
    for (LocalBitmap *bitmap : local_bitmaps) {
        M_insist(~bitmap->bitmask == 0, "all bits must have been deallocated");
        delete bitmap;
    }
    local_bitmaps_stack_.pop_back();
}

inline LocalBit Module::allocate_bit()
{
    auto &local_bitmaps = local_bitmaps_stack_.back();

    if (local_bitmaps.empty())
        local_bitmaps.emplace_back(new LocalBitmap()); // allocate new local bitmap in current function

    LocalBitmap &bitmap = *local_bitmaps.back();
    M_insist(bitmap.bitmask, "bitmap must have at least one bit unoccupied");

    uint8_t bit_offset = __builtin_ctzl(bitmap.bitmask);
    bitmap.bitmask ^= 1UL << bit_offset; // clear allocated bit

    LocalBit bit(bitmap, bit_offset);

    if (bitmap.bitmask == 0) // all bits have been allocated
        local_bitmaps.pop_back(); // remove bitmap entry, ownership transitions to *all* referencing `LocalBit`s

    return bit;
}

template<typename T>
inline PrimitiveExpr<T> Module::get_global(const char *name)
{
    return PrimitiveExpr<T>(builder_.makeGlobalGet(name, wasm_type<T>()));
}

template<typename ReturnType, typename... ParamTypes>
requires std::is_void_v<ReturnType>
inline void Module::emit_call(const char *fn, PrimitiveExpr<ParamTypes>... args)
{
    active_block_->list.push_back(
        builder_.makeCall(fn, { args.expr()... }, wasm_type<ReturnType>())
    );
}

template<typename ReturnType, typename... ParamTypes>
requires dsl_primitive<ReturnType> or dsl_pointer_to_primitive<ReturnType>
inline PrimitiveExpr<ReturnType> Module::emit_call(const char *fn, PrimitiveExpr<ParamTypes>... args)
{
    return PrimitiveExpr<ReturnType>(
        builder_.makeCall(fn, { args.expr()... }, wasm_type<ReturnType>())
    );
}

inline void Module::emit_return()
{
    active_block_->list.push_back(builder_.makeReturn());
}

template<typename T>
inline void Module::emit_return(PrimitiveExpr<T> value)
{
    active_block_->list.push_back(builder_.makeReturn(value.expr()));
}

template<typename T>
inline void Module::emit_return(Expr<T> value)
{
    emit_return(value.insist_not_null());
}

/** Emit an unconditional break, breaking \p level levels. */
inline void Module::emit_break(std::size_t level)
{
    M_insist(level > 0);
    M_insist(branch_target_stack_.size() >= level);
    auto &branch_targets = branch_target_stack_[branch_target_stack_.size() - level];
    active_block_->list.push_back(builder_.makeBreak(branch_targets.brk));
}

/** Emit a conditional break, breaking if \p cond is `true` and breaking \p level levels. */
inline void Module::emit_break(PrimitiveExpr<bool> cond, std::size_t level)
{
    M_insist(level > 0);
    M_insist(branch_target_stack_.size() >= level);
    auto &branch_targets = branch_target_stack_[branch_target_stack_.size() - level];
    active_block_->list.push_back(builder_.makeBreak(branch_targets.brk, nullptr, cond.expr()));
}

template<typename T>
PrimitiveExpr<T> Module::emit_select(PrimitiveExpr<bool> cond, PrimitiveExpr<T> tru, PrimitiveExpr<T> fals)
{
    return PrimitiveExpr<T>(builder_.makeSelect(cond.expr(), tru.expr(), fals.expr()));
}

template<typename T>
Expr<T> Module::emit_select(PrimitiveExpr<bool> cond, Expr<T> tru, Expr<T> fals)
{
    if (tru.can_be_null() or fals.can_be_null()) {
        auto [tru_val, tru_is_null] = tru.split();
        auto [fals_val, fals_is_null] = fals.split();
        auto cond_cloned = cond.clone();
        return Expr<T>(
            /* value=   */ PrimitiveExpr<T>(builder_.makeSelect(cond_cloned.expr(), tru_val.expr(), fals_val.expr())),
            /* is_null= */ PrimitiveExpr<bool>(builder_.makeSelect(cond.expr(), tru_is_null.expr(),
                                                                                fals_is_null.expr()))
        );
    } else {
        return Expr<T>(
            /* value=   */ PrimitiveExpr<T>(builder_.makeSelect(cond.expr(), tru.insist_not_null().expr(),
                                                                             fals.insist_not_null().expr()))
        );
    }
}

inline void Module::push_branch_targets(::wasm::Name brk, ::wasm::Name continu, PrimitiveExpr<bool> condition)
{
    branch_target_stack_.emplace_back(brk, continu, condition.expr());
}


/*======================================================================================================================
 * Specialization of `variable_storage` for local, non-`NULL` boolean.
 *====================================================================================================================*/

namespace detail {

/*----- Specialization for local variables of boolean type that *cannot* be `NULL`. ----------------------------------*/

inline variable_storage<bool, VariableKind::Local, false>::variable_storage()
    : value_(std::make_shared<LocalBit>(Module::Get().allocate_bit()))
{ }

template<primitive_convertible U>
requires requires (U &&u) { PrimitiveExpr<bool>(primitive_expr_t<U>(std::forward<U>(u))); }
void variable_storage<bool, VariableKind::Local, false>::operator=(U &&u)
{
    PrimitiveExpr<bool> value(primitive_expr_t<U>(std::forward<U>(u)));
    value_->set(value);
}

inline variable_storage<bool, VariableKind::Local, false>::operator PrimitiveExpr<bool>() const
{
    return PrimitiveExpr<bool>(/* expr= */ value_->is_set().expr(), /* referenced_bits= */ { value_ });
}

}


/*======================================================================================================================
 * LocalBit
 *====================================================================================================================*/

inline LocalBit::~LocalBit()
{
    if (bitmap_) {
        M_insist((bitmap_->bitmask bitand mask()) == 0, "bit must still be allocated");

        if (bitmap_->bitmask == 0) // empty bitmap
            Module::Get().local_bitmaps_stack_.back().emplace_back(bitmap_); // make discoverable again

        bitmap_->bitmask |= mask(); // deallocate bit
    }
}

inline PrimitiveExpr<bool> LocalBit::is_set() const { return (bitmap_->u64 bitand mask()).to<bool>(); }

inline void LocalBit::set() { bitmap_->u64 |= mask(); }

inline void LocalBit::clear() { bitmap_->u64 &= ~mask(); }

inline void LocalBit::set(PrimitiveExpr<bool> value)
{
    bitmap_->u64 = (bitmap_->u64 bitand ~mask()) bitor (value.to<uint64_t>() << offset());
}

inline LocalBit & LocalBit::operator=(const LocalBit &other)
{
    auto other_bit = other.bitmap_->u64 bitand other.mask();
    Var<U64> this_bit;

    if (this->offset() > other.offset()) {
        const auto shift_width = this->offset() - other.offset();
        this_bit = other_bit << shift_width;
    } else if (other.offset() > this->offset()) {
        const auto shift_width = other.offset() - this->offset();
        this_bit = other_bit >> shift_width;
    } else {
        this_bit = other_bit;
    }

    this->bitmap_->u64 = (this->bitmap_->u64 bitand ~this->mask()) bitor this_bit; // clear, then set bit

    return *this;
}

#undef UNARY_LIST
#undef BINARY_LIST
#undef ASSIGNOP_LIST

}

}
