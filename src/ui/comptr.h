#pragma once

// MinGW ships no WRL, so this is the small slice of ComPtr the renderer needs.
template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ComPtr(const ComPtr& other) : ptr_(other.ptr_)
    {
        if (ptr_)
        {
            ptr_->AddRef();
        }
    }
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ~ComPtr() { reset(); }

    ComPtr& operator=(const ComPtr& other)
    {
        if (this != &other)
        {
            if (other.ptr_)
            {
                other.ptr_->AddRef();
            }
            reset();
            ptr_ = other.ptr_;
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    // For CreateXxx(..., &out) style calls; releases any previous value first.
    T** put()
    {
        reset();
        return &ptr_;
    }

    void** putVoid()
    {
        reset();
        return reinterpret_cast<void**>(&ptr_);
    }

    void attach(T* raw)
    {
        reset();
        ptr_ = raw;
    }

    T* detach()
    {
        T* raw = ptr_;
        ptr_ = nullptr;
        return raw;
    }

    void reset()
    {
        if (ptr_)
        {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};
