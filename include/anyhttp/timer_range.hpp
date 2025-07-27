#include <chrono>
#include <ranges>

template <typename Unit = std::chrono::milliseconds>
class TimerRange
{
public:
   class iterator
   {
   public:
      using value_type = Unit;
      using difference_type = std::ptrdiff_t;
      using iterator_concept = std::input_iterator_tag;

      iterator() : last_time_(std::chrono::steady_clock::now()) {}

      value_type operator*() const { return elapsed_; }

      iterator& operator++()
      {
         auto now = std::chrono::steady_clock::now();
         elapsed_ = std::chrono::duration_cast<value_type>(now - last_time_);
         last_time_ = now; // Update last_time_ after calculating elapsed
         return *this;
      }
      void operator++(int) { ++(*this); }

      bool operator==(std::default_sentinel_t) const { return false; } // infinite

   private:
      std::chrono::steady_clock::time_point last_time_;
      value_type elapsed_{0};
   };

   iterator begin() const { return iterator(); }
   std::default_sentinel_t end() const { return std::default_sentinel; }
};

static_assert(std::ranges::range<TimerRange<>>);
