//
// ThreadSanitizer cannot see the synchronization in libc++'s weak_ptr: __release_weak() is not
// inline but in libc++.so, which is not instrumented. An object made with make_shared() whose
// last weak_ptr goes away on another thread than the one that destroyed it -- a session and a
// timer callback holding a weak_ptr to it, on two strands -- is then reported as a race between
// its destructor and the free of its memory.
//
// This is the same function, compiled with the instrumentation: the definition in the executable
// takes precedence over the one in libc++.so, and TSAN sees the acquire and the decrement.
//
#include <memory> // IWYU pragma: keep (_LIBCPP_VERSION, and what is defined here)

#if defined(__has_feature) && defined(_LIBCPP_VERSION)
#if __has_feature(thread_sanitizer)

void std::__shared_weak_count::__release_weak() noexcept
{
   // the last weak owner needs no decrement, libc++ skips it too
   if (__atomic_load_n(&__shared_weak_owners_, __ATOMIC_ACQUIRE) == 0)
      __on_zero_shared_weak();
   else if (__libcpp_atomic_refcount_decrement(__shared_weak_owners_) == -1)
      __on_zero_shared_weak();
}

#endif
#endif
