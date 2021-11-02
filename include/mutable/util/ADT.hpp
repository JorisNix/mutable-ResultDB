#pragma once

#include "mutable/util/allocator.hpp"
#include "mutable/util/exception.hpp"
#include "mutable/util/fn.hpp"
#include "mutable/util/macro.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>


namespace m {

/** Implements a small and efficient set over integers in the range of `0` to `63` (including). */
struct SmallBitset
{
    static constexpr std::size_t CAPACITY = 64; ///< the maximum capacity of a `SmallBitset`

    /** A proxy to access single elements in `SmallBitset`. */
    template<bool C>
    struct Proxy
    {
        static constexpr bool Is_Const = C;

        private:
        using reference_type = std::conditional_t<Is_Const, const SmallBitset&, SmallBitset&>;

        reference_type S_;
        std::size_t offset_;

        public:
        Proxy(reference_type S, std::size_t offset) : S_(S), offset_(offset) { insist(offset_ < CAPACITY); }

        operator bool() const { return (S_.bits_ >> offset_) & 0b1; }

        template<bool C_ = Is_Const>
        std::enable_if_t<not C_, Proxy&>
        operator=(bool val) { setbit(&S_.bits_, val, offset_); return *this; }
    };

    private:
    uint64_t bits_; ///< the bit vector representing the set

    struct iterator
    {
        private:
        uint64_t bits_;

        public:
        iterator(uint64_t bits) : bits_(bits) { }

        bool operator==(iterator other) const { return this->bits_ == other.bits_; }
        bool operator!=(iterator other) const { return not operator==(other); }

        iterator & operator++() { bits_ = bits_ & (bits_ - 1); /* reset lowest set bit */ return *this; }
        iterator operator++(int) { auto clone = *this; ++clone; return clone; }

        std::size_t operator*() const { insist(bits_ != 0); return __builtin_ctzl(bits_); }
    };

    public:
    SmallBitset() : bits_(0) { };
    explicit SmallBitset(uint64_t bits) : bits_(bits) { };

    /** Returns the `offset`-th bit.  Requires that `offset` is in range `[0; CAPACITY)`. */
    Proxy<true> operator()(std::size_t offset) const { return Proxy<true>(*this, offset); }

    /** Returns the `offset`-th bit.  Requires that `offset` is in range `[0; CAPACITY)`. */
    Proxy<false> operator()(std::size_t offset) { return Proxy<false>(*this, offset); }

    /** Returns the `offset`-th bit.  Requires that `offset` is in range `[0; CAPACITY)`. */
    Proxy<true> operator[](std::size_t offset) const { return operator()(offset); }

    /** Returns the `offset`-th bit.  Requires that `offset` is in range `[0; CAPACITY)`. */
    Proxy<false> operator[](std::size_t offset) { return operator()(offset); }

    /** Returns a proxy to the bit at offset `offset`.  Throws `m::out_of_range` if `offset` is not in range
     * `[0; CAPACITY)`. */
    Proxy<true> at(std::size_t offset) const {
        if (offset >= CAPACITY)
            throw m::out_of_range("offset is out of bounds");
        return operator()(offset);
    }

    /** Returns a proxy to the bit at offset `offset`.  Throws `m::out_of_range` if `offset` is not in range
     * `[0; CAPACITY)`. */
    Proxy<false> at(std::size_t offset) {
        if (offset >= CAPACITY)
            throw m::out_of_range("offset is out of bounds");
        return operator()(offset);
    }

    /** Returns the maximum capacity. */
    constexpr std::size_t capacity() { return CAPACITY; }
    /** Returns the number of elements in this `SmallBitset`. */
    std::size_t size() const { return __builtin_popcountl(bits_); }
    /** Returns `true` if there are no elements in this `SmallBitset`. */
    bool empty() const { return size() == 0; }

    auto begin() const { return iterator(bits_); }
    auto cbegin() const { return begin(); }
    auto end() const { return iterator(0); }
    auto cend() const { return end(); }

    /** Convert the `SmallBitset` type to `uint64_t`. */
    explicit operator uint64_t() const { return bits_; }
    explicit operator bool() const { return not empty(); }

    bool operator==(SmallBitset other) const { return this->bits_ == other.bits_; }
    bool operator!=(SmallBitset other) const { return not operator==(other); }

    /** Inverts all bits in the bitset. */
    SmallBitset operator~() const { return SmallBitset(~bits_); }

    /** Returns `true` if the set represented by `this` is a subset of `other`, i.e.\ `this` ⊆ `other`. */
    bool is_subset(SmallBitset other) const { return this->bits_ == (other.bits_ & this->bits_); }

    /** Converts a singleton set to a mask for all bits lower than the single, set bit. */
    SmallBitset singleton_to_lo_mask() const {
        insist((bits_ & (bits_ - 1UL)) == 0UL, "not a singleton set");
        return bits_ ? SmallBitset(bits_ - 1UL) : SmallBitset(0UL);
    }

    /** Returns the union of `left` and `right`, i.e.\ `left` ∪ `right`. */
    friend SmallBitset unify(SmallBitset left, SmallBitset right) { return SmallBitset(left.bits_ | right.bits_); }
    /** Returns the intersection of `left` and `right`, i.e.\ `left` ∩ `right`. */
    friend SmallBitset intersect(SmallBitset left, SmallBitset right) { return SmallBitset(left.bits_ & right.bits_); }
    /** Returns the set where the elements of `left` have been subtracted from `right`, i.e.\ `left` - `right`. */
    friend SmallBitset subtract(SmallBitset left, SmallBitset right) { return SmallBitset(left.bits_ & ~right.bits_); }
    /** Returns the union of `left` and `right`, i.e.\ `left` ∪ `right`. */
    friend SmallBitset operator|(SmallBitset left, SmallBitset right) { return unify(left, right); }
    /** Returns the intersection of `left` and `right`, i.e.\ `left` ∩ `right`. */
    friend SmallBitset operator&(SmallBitset left, SmallBitset right) { return intersect(left, right); }
    /** Returns the set where the elements of `right` have been subtracted from `left`, i.e.\ `left` - `right`. */
    friend SmallBitset operator-(SmallBitset left, SmallBitset right) { return subtract(left, right); }

    SmallBitset & operator|=(SmallBitset other) { return *this = *this | other; }
    SmallBitset & operator&=(SmallBitset other) { return *this = *this & other; }
    SmallBitset & operator-=(SmallBitset other) { return *this = *this - other; }

    /** Write a textual representation of `s` to `out`. */
    friend std::ostream & operator<<(std::ostream &out, SmallBitset s) {
        for (uint64_t i = CAPACITY; i --> 0;)
            out << s(i);
        return out;
    }

    /** Print a textual representation of `this` with `size` bits to `out`. */
    void print_fixed_length(std::ostream &out, std::size_t size) const {
        for (uint64_t i = size; i --> 0;)
            out << (*this)(i);
    }

    void dump(std::ostream &out) const;
    void dump() const;
};


/** Returns the least subset of a given `set`, i.e.\ the set represented by the lowest 1 bit. */
inline SmallBitset least_subset(SmallBitset S) { return SmallBitset(uint64_t(S) & -uint64_t(S)); }

/** Returns the next subset of a given `subset` and `set. */
inline SmallBitset next_subset(SmallBitset subset, SmallBitset set)
{
    return SmallBitset(uint64_t(subset) - uint64_t(set)) & set;
}

/** Implements an array of dynamic but fixed size. */
template<typename T>
struct dyn_array
{
    using value_type = T;
    using size_type = std::size_t;
    using iterator = value_type*;
    using const_iterator = const value_type*;

    private:
    std::unique_ptr<value_type[]> arr_;
    std::size_t size_;

    public:
    /** Constructs an array of size 0. */
    dyn_array() : size_(0) { }

    /** Constructs an array of size `size`. */
    explicit dyn_array(std::size_t size)
        : arr_(std::make_unique<value_type[]>(size))
        , size_(size)
    { }

    /** Constructs an array with the elements in range `[begin, end)`.  The size of the array will be
     * `std::distance(begin, end)`.  */
    template<typename InputIt>
    dyn_array(InputIt begin, InputIt end)
        : dyn_array(std::distance(begin, end))
    {
        auto ptr = data();
        for (auto it = begin; it != end; ++it)
            new (ptr++) value_type(*it);
    }

    /** Copy-constructs an array. */
    explicit dyn_array(const dyn_array &other)
        : dyn_array(other.size())
    {
        for (std::size_t i = 0; i != other.size(); ++i)
            new (&data()[i]) value_type(other[i]);
    }

    dyn_array(dyn_array&&) = default;
    dyn_array & operator=(dyn_array&&) = default;

    /** Returns the size of this array, i.e. the number of elements. */
    size_type size() const { return size_; }

    /** Returns a pointer to the beginning of the array. */
    const value_type * data() const { return arr_.get(); }
    /** Returns a pointer to the beginning of the array. */
    value_type * data() { return arr_.get(); }

    /** Returns a reference to the element at position `pos`.  Requires that `pos` is in bounds. */
    const value_type & operator[](std::size_t pos) const {
        insist(pos < size(), "index out of bounds");
        return data()[pos];
    }

    /** Returns a reference to the element at position `pos`.  Requires that `pos` is in bounds. */
    value_type & operator[](std::size_t pos) {
        insist(pos < size(), "index out of bounds");
        return data()[pos];
    }

    /** Returns a reference to the element at position `pos`.  Throws `m::out_of_range` if `pos` is out of bounds. */
    const value_type & at(std::size_t pos) const {
        if (pos >= size())
            throw m::out_of_range("index out of bounds");
        return (*this)[pos];
    }

    /** Returns a reference to the element at position `pos`.  Throws `m::out_of_range` if `pos` is out of bounds. */
    value_type & at(std::size_t pos) {
        if (pos >= size())
            throw m::out_of_range("index out of bounds");
        return (*this)[pos];
    }

    iterator begin() { return data(); }
    iterator end() { return data() + size(); }
    const_iterator begin() const { return data(); }
    const_iterator end() const { return data() + size(); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    /** Returns `true` iff the contents of `this` and `other` are equal, that is, they have the same number of elements
     * and each element in `this` compares equal with the element in `other` at the same position. */
    bool operator==(const dyn_array &other) const {
        if (this->size() != other.size())
            return false;

        for (std::size_t i = 0; i != size(); ++i) {
            if ((*this)[i] != other[i])
                return false;
        }

        return true;
    }
    /** Returns `false` iff the contents of `this` and `other` are equal, that is, they have the same number of elements
     * and each element in `this` compares equal with the element in `other` at the same position. */
    bool operator!=(const dyn_array &other) const { return not operator==(other); }
};

/** Implements a doubly-linked list with an overhead of just a single pointer per element. */
template<
    typename T,
    typename Allocator = malloc_allocator
>
struct doubly_linked_list
{
    using value_type = T;
    using size_type = std::size_t;
    using allocator_type = Allocator;

    using reference_type = T&;
    using const_reference_type = const T&;

    private:
    ///> the memory allocator
    allocator_type &&allocator_;

    struct elem_t
    {
        std::uintptr_t ptrxor_;
        T value_;
    };

    ///> points to the first element
    elem_t *head_ = nullptr;
    ///> points to the last element
    elem_t *tail_ = nullptr;
    ///> the number of elements in the list
    size_type size_ = 0;

    template<bool C>
    struct the_iterator
    {
        friend struct doubly_linked_list;

        static constexpr bool Is_Const = C;

        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = std::size_t;
        using pointer = std::conditional_t<Is_Const, const T*, T*>;
        using reference = std::conditional_t<Is_Const, const T&, T&>;

        private:
        elem_t *elem_;
        std::uintptr_t prev_;

        public:
        the_iterator(elem_t *elem, std::uintptr_t prev) : elem_(elem), prev_(prev) { }

        the_iterator & operator++() {
            insist(elem_, "cannot advance a past-the-end iterator");
            elem_t *curr = elem_;
            elem_ = reinterpret_cast<elem_t*>(prev_ ^ elem_->ptrxor_);
            prev_ = reinterpret_cast<std::uintptr_t>(curr);
            return *this;
        }

        the_iterator operator++(int) { the_iterator clone = *this; operator++(); return clone; }

        the_iterator & operator--() {
            elem_t *prev = reinterpret_cast<elem_t*>(prev_);
            insist(prev, "cannot retreat past the beginning");
            prev_ = prev->ptrxor_ ^ reinterpret_cast<std::uintptr_t>(elem_);
            elem_ = prev;
            return *this;
        }

        the_iterator operator--(int) { the_iterator clone = *this; operator--(); return clone; }

        reference operator*() const { return elem_->value_; }
        pointer operator->() const { return &elem_->value_; }

        bool operator==(const the_iterator &other) const {
            return this->elem_ == other.elem_ and this->prev_ == other.prev_;
        }
        bool operator!=(const the_iterator &other) const { return not operator==(other); }

        operator the_iterator<true>() const { return the_iterator<true>(elem_, prev_); }
    };

    public:
    using iterator = the_iterator<false>;
    using const_iterator = the_iterator<true>;

    friend void swap(doubly_linked_list &first, doubly_linked_list &second) {
        using std::swap;
        swap(first.head_, second.head_);
        swap(first.tail_, second.tail_);
        swap(first.size_, second.size_);
    }

    /*----- Constructors & Destructor --------------------------------------------------------------------------------*/
    doubly_linked_list() : doubly_linked_list(allocator_type()) { }
    explicit doubly_linked_list(allocator_type &&allocator)
        : allocator_(std::forward<allocator_type>(allocator))
    { }

    template<typename InputIt>
    doubly_linked_list(InputIt begin, InputIt end, allocator_type &&allocator = allocator_type())
        : allocator_(std::forward<allocator_type>(allocator))
    {
        for (auto it = begin; it != end; ++it)
            push_back(*it);
    }

    ~doubly_linked_list() { clear(); }

    explicit doubly_linked_list(const doubly_linked_list &other) : doubly_linked_list() {
        insert(begin(), other.cbegin(), other.cend());
    }

    doubly_linked_list(doubly_linked_list &&other) : doubly_linked_list() { swap(*this, other); }

    doubly_linked_list & operator=(doubly_linked_list other) { swap(*this, other); return *this; }

    /*----- Element access -------------------------------------------------------------------------------------------*/
    reference_type front() { insist(head_); return head_->value_; }
    const_reference_type front() const { insist(head_); return head_->value_; }
    reference_type back() { insist(tail_); return tail_->value_; }
    const_reference_type back() const { insist(tail_); return tail_->value_; }

    /*----- Iterators ------------------------------------------------------------------------------------------------*/
    iterator begin() { return iterator(head_, 0); }
    iterator end() { return iterator(nullptr, reinterpret_cast<std::uintptr_t>(tail_)); }
    const_iterator begin() const { return const_iterator(head_, 0); }
    const_iterator end() const { return const_iterator(nullptr, reinterpret_cast<std::uintptr_t>(tail_)); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    iterator rbegin() { return iterator(tail_, 0); }
    iterator rend() { return iterator(nullptr, reinterpret_cast<std::uintptr_t>(head_)); }
    const_iterator rbegin() const { return const_iterator(tail_, 0); }
    const_iterator rend() const { return const_iterator(nullptr, reinterpret_cast<std::uintptr_t>(head_)); }
    const_iterator crbegin() const { return rbegin(); }
    const_iterator crend() const { return rend(); }

    /*----- Capacity -------------------------------------------------------------------------------------------------*/
    bool empty() const { return size_ == 0; }
    size_type size() const { return size_; }
    size_type max_size() const { return std::numeric_limits<size_type>::max(); }

    /*----- Modifiers ------------------------------------------------------------------------------------------------*/
    template<typename... Args>
    iterator emplace(const_iterator pos, Args&&... args) {
        elem_t *new_elem = allocate_elem();
        new (&new_elem->value_) value_type(std::forward<Args>(args)...);
        new_elem->ptrxor_ = pos.prev_ ^ reinterpret_cast<std::uintptr_t>(pos.elem_);

        elem_t *prev = reinterpret_cast<elem_t*>(pos.prev_);
        if (prev)
            prev->ptrxor_ ^= reinterpret_cast<std::uintptr_t>(pos.elem_) ^ reinterpret_cast<std::uintptr_t>(new_elem);
        else // insert at front
            head_ = new_elem;
        if (pos.elem_)
            pos.elem_->ptrxor_ ^= pos.prev_ ^ reinterpret_cast<std::uintptr_t>(new_elem);
        else // insert at end
            tail_ = new_elem;

        ++size_;
        return iterator(new_elem, pos.prev_);
    }

    template<typename... Args>
    reference_type emplace_back(Args&&... args) {
        auto it = emplace(end(), std::forward<Args>(args)...);
        return *it;
    }

    template<typename... Args>
    reference_type emplace_front(Args&&... args) {
        auto it = emplace(begin(), std::forward<Args>(args)...);
        return *it;
    }

    void push_back(const value_type &value) { emplace_back(value); }
    void push_back(value_type &&value) { emplace_back(std::move(value)); }
    void push_front(const value_type &value) { emplace_front(value); }
    void push_front(value_type &&value) { emplace_front(std::move(value)); }

    iterator insert(const_iterator pos, const value_type &value) { return emplace(pos, value); }
    iterator insert(const_iterator pos, value_type &&value) { return emplace(pos, std::move(value)); }
    iterator insert(const_iterator pos, size_type count, const value_type &value) {
        iterator it(pos.elem_, pos.prev_);
        while (count--) it = insert(it, value);
        return it;
    }

    template<typename InputIt,
             typename = decltype(*std::declval<InputIt&>(), std::declval<InputIt&>()++, void())>
    iterator insert(const_iterator pos, InputIt first, InputIt last) {
        if (first == last) return iterator(pos.elem_, pos.prev_);

        iterator begin = insert(pos, *first++);
        insist(begin != end());
        insist(begin.elem_);
        iterator it = begin;
        while (first != last) it = insert(++it, *first++);

        return begin;
    }

    iterator insert(const_iterator pos, std::initializer_list<value_type> ilist) {
        return insert(pos, ilist.begin(), ilist.end());
    }

    iterator erase(iterator pos) {
        insist(pos.elem_);
        insist(size_);
        elem_t *prev = reinterpret_cast<elem_t*>(pos.prev_);
        elem_t *next = reinterpret_cast<elem_t*>(pos.elem_->ptrxor_ ^ pos.prev_);
        if (prev)
            prev->ptrxor_ ^= reinterpret_cast<std::uintptr_t>(pos.elem_) ^ reinterpret_cast<std::uintptr_t>(next);
        else // erased first element
            head_ = next;
        if (next)
            next->ptrxor_ ^= reinterpret_cast<std::uintptr_t>(pos.elem_) ^ reinterpret_cast<std::uintptr_t>(prev);
        else // erased last element
            tail_ = prev;
        deallocate_elem(pos.elem_);
        --size_;
        return iterator(next, pos.prev_);
    }

    iterator erase(const_iterator pos) { return erase(iterator(pos.elem_, pos.prev_)); }

    value_type pop_back() {
        reverse();
        value_type value = pop_front();
        reverse();
        return value;
    }

    value_type pop_front() {
        insist(head_);
        insist(tail_);
        insist(size_);
        value_type value = std::move(head_->value_);
        erase(begin());
        return value;
    }

    void clear() { while (head_) pop_front(); insist(size_ == 0); }

    void swap(doubly_linked_list &other) { swap(*this, other); }

    /*----- Operations -----------------------------------------------------------------------------------------------*/
    void reverse() { std::swap(head_, tail_); }

    private:
    elem_t * allocate_elem() { return allocator_.template allocate<elem_t>(); }
    void deallocate_elem(elem_t *ptr) { allocator_.template deallocate<elem_t>(ptr); }
};

}
