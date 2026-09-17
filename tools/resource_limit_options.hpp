#pragma once
#include <psizam/detail/execution_limits.hpp>
#include <charconv>
#include <string_view>
#include <stdexcept>

// Shared CLI spelling used by pzam-run and the reproducible benchmark runner.
inline uint64_t limit_number(std::string_view value) {
   uint64_t n{};
   auto [end, ec] = std::from_chars(value.data(), value.data()+value.size(), n);
   if (ec != std::errc{} || end != value.data()+value.size() || n == 0 || n > INT64_MAX)
      throw std::invalid_argument("resource budget must be a positive signed 64-bit integer");
   return n;
}
inline bool parse_limit_option(std::string_view arg, psizam::detail::execution_limits& limits) {
   using psizam::detail::cpu_limit;
   if (arg.starts_with("--cpu-limit=")) {
      auto value = arg.substr(12);
      if (value == "none") limits.policy = cpu_limit::none;
      else if (value == "gas") limits.policy = cpu_limit::gas;
      else if (value == "timeout") limits.policy = cpu_limit::timeout;
      else if (value == "timeout-poll") limits.policy = cpu_limit::timeout_poll;
      else throw std::invalid_argument("--cpu-limit must be none, gas, timeout, or timeout-poll");
   } else if (arg.starts_with("--gas-budget=")) limits.gas_budget = limit_number(arg.substr(13));
   else if (arg.starts_with("--timeout-ms=")) {
      auto n = limit_number(arg.substr(13));
      if (n > 86'400'000) throw std::invalid_argument("timeout cannot exceed one day");
      limits.duration = std::chrono::milliseconds(n);
   } else if (arg.starts_with("--poll-slice=")) limits.poll_slice = limit_number(arg.substr(13));
   else return false;
   return true;
}
