#include <psizam/detail/execution_context.hpp>

int main() {
   const auto bounds = psizam::detail::current_thread_stack_bounds();
   if (bounds.low || bounds.high || psizam::detail::available_native_stack()) return 1;
   psizam::detail::stack_manager manager;
   return manager.execute(1024, [](auto& stack) { return stack.top() ? 1 : 0; });
}
