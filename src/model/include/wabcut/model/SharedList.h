#pragma once

// A list held by a shared pointer and copied only when written (C.10). ProjectState is an immutable snapshot, and every
// edit and every undo step keeps one: a project-level collection held by value is copied whole each time (1,000 review
// notes cost about 300 KB per undo step). Snapshots share a SharedList until one of them is changed.
//
// It behaves like a std::vector<T>. Through a const object (every published snapshot) it only reads. Through a non-const
// one, edit() and every access that can write - begin()/end(), [], front()/back(), the mutators below - first make the
// list this object's own (a copy when another snapshot still holds it), so code written for a vector stays correct; a
// read-only loop over a non-const state merely copies a list it did not need to. Positions given to insert()/erase() are
// iterators of this list; they become indexes before any copy.

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace wabcut {

template <class T>
class SharedList {
public:
    using value_type = T;
    using size_type = std::size_t;
    using const_iterator = typename std::vector<T>::const_iterator;
    using iterator = typename std::vector<T>::iterator;
    using const_reference = const T&;
    using reference = T&;

    SharedList() = default;
    SharedList(std::vector<T> items) // NOLINT(google-explicit-constructor): assigning a vector is the point
        : m_items(items.empty() ? nullptr : std::make_shared<std::vector<T>>(std::move(items))) {}
    SharedList(std::initializer_list<T> items) : SharedList(std::vector<T>(items)) {}

    const std::vector<T>& get() const noexcept { return m_items ? *m_items : noItems(); }
    operator const std::vector<T>&() const noexcept { return get(); } // NOLINT(google-explicit-constructor)

    const_iterator begin() const noexcept { return get().begin(); }
    const_iterator end() const noexcept { return get().end(); }
    const_iterator cbegin() const noexcept { return get().begin(); }
    const_iterator cend() const noexcept { return get().end(); }
    size_type size() const noexcept { return get().size(); }
    bool empty() const noexcept { return get().empty(); }
    const T& operator[](size_type i) const { return get()[i]; }
    const T& front() const { return get().front(); }
    const T& back() const { return get().back(); }
    const T* data() const noexcept { return get().data(); }

    /// The items to change, this object's own (copied first when another snapshot still holds them).
    std::vector<T>& edit() {
        if (!m_items) {
            m_items = std::make_shared<std::vector<T>>();
        } else if (m_items.use_count() > 1) {
            m_items = std::make_shared<std::vector<T>>(*m_items);
        }
        return *m_items;
    }

    // Writable access, as on a vector (each makes the list this object's own first).
    iterator begin() { return edit().begin(); }
    iterator end() { return edit().end(); }
    T& operator[](size_type i) { return edit()[i]; }
    T& front() { return edit().front(); }
    T& back() { return edit().back(); }

    void push_back(T item) { edit().push_back(std::move(item)); }
    template <class... Args>
    T& emplace_back(Args&&... args) {
        return edit().emplace_back(std::forward<Args>(args)...);
    }
    T& insert(const_iterator position, T item) {
        const auto index = position - cbegin();
        std::vector<T>& items = edit();
        return *items.insert(items.begin() + index, std::move(item));
    }
    void erase(const_iterator position) {
        const auto index = position - cbegin();
        std::vector<T>& items = edit();
        items.erase(items.begin() + index);
    }
    void clear() { m_items.reset(); }

    /// Equal items; the same shared list is equal without comparing them.
    bool operator==(const SharedList& other) const { return m_items == other.m_items || get() == other.get(); }
    /// Both hold the very same list (an unchanged collection between two snapshots).
    bool sharesWith(const SharedList& other) const noexcept { return m_items == other.m_items; }

private:
    static const std::vector<T>& noItems() {
        static const std::vector<T> none;
        return none;
    }

    std::shared_ptr<std::vector<T>> m_items;
};

} // namespace wabcut
