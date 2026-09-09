#!/usr/bin/env python3
"""Exercise production custom-mode publication with staged KScreen responses.

Requires a C++20 compiler and nlohmann/json.hpp (pass its parent via --include-dir).
Only the external configuration-query/wait boundary is replaced.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[4]
source = (ROOT / "src/platform/linux/private_display.cpp").read_text()


def function(name):
    match = re.search(
        r"^    [^\n]*\b" + name + r"\([\s\S]*?^    \}", source, re.M
    )
    if not match:
        raise RuntimeError("Missing production function: " + name)
    return match.group()


request_type = re.search(
    r"^    struct custom_mode_request_t \{[\s\S]*?^    \};", source, re.M
).group()
program = r'''
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>
#include "src/platform/linux/private_display_mode_policy.h"
using json = nlohmann::json;
namespace mode_policy = platf::linux_private_display::mode_policy;
namespace display_device {
  struct Resolution { unsigned m_width, m_height; };
  struct Rational { unsigned m_numerator, m_denominator; };
  using FloatingPoint = std::variant<double, Rational>;
}
std::vector<json> responses;
unsigned queries;
template<class Predicate> bool wait_for_configuration(Predicate &&predicate) {
  for (const auto &response : responses) {
    ++queries;
    if (predicate(response)) return true;
  }
  return false;
}
''' + "\n".join(function(name) for name in (
    "find_output", "connected", "floating_point", "best_mode_id", "mode_matches_refresh"
)) + request_type + function("wait_for_custom_modes") + r'''

json output(const std::string &name, const unsigned width, const unsigned height,
            const double refresh, const bool connected = true) {
  return {{"name", name}, {"connected", connected}, {"modes", json::array({
    {{"id", "mode"}, {"size", {{"width", width}, {"height", height}}}, {"refreshRate", refresh}}
  })}};
}
json configuration(json::initializer_list_t outputs) {
  return {{"outputs", json::array(outputs)}};
}
int main() {
  const custom_mode_request_t request {"Virtual-1", {1920, 1200}, 138.0};
  const auto stale = configuration({output("Virtual-1", 1920, 1200, 60)});
  const auto correct = configuration({output("Virtual-1", 1920, 1200, 138)});

  // Reproduce the reported 1920x1200@138 request returning the old 60 Hz mode.
  responses = {stale, correct}; queries = 0;
  auto result = wait_for_custom_modes({request});
  assert(result == correct && queries == 2);

  responses = {stale, stale}; queries = 0;
  assert(!wait_for_custom_modes({request}) && queries == 2);

  // Matching refresh on a different resolution is not publication success.
  responses = {configuration({output("Virtual-1", 1920, 1080, 138)}), correct};
  queries = 0;
  assert(wait_for_custom_modes({request}) == correct && queries == 2);

  responses = {configuration({}),
    configuration({output("Virtual-1", 1920, 1200, 138, false)}), correct};
  queries = 0;
  assert(wait_for_custom_modes({request}) == correct && queries == 3);

  const auto fractional = configuration({output("Virtual-1", 1920, 1200, 137.99)});
  responses = {fractional}; queries = 0;
  assert(wait_for_custom_modes({request}) == fractional && queries == 1);

  // A Remote Monitor batch must publish every custom mode in one snapshot.
  const custom_mode_request_t second {"Virtual-2", {2560, 1600}, 144.0};
  const auto both = configuration({output("Virtual-1", 1920, 1200, 138),
    output("Virtual-2", 2560, 1600, 144)});
  responses = {correct, configuration({output("Virtual-2", 2560, 1600, 144)}), both};
  queries = 0;
  assert(wait_for_custom_modes({request, second}) == both && queries == 3);
  std::cout << "6 custom-mode publication regressions passed.\n";
}
'''

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", default="c++")
parser.add_argument("--include-dir", action="append", default=[])
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="private-display-modes-") as directory:
    directory = Path(directory)
    harness = directory / "test.cpp"
    binary = directory / "test"
    harness.write_text(program)
    includes = [f"-I{ROOT}"] + [f"-I{path}" for path in args.include_dir]
    subprocess.run([
        args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", *includes,
        str(harness), "-o", str(binary)
    ], check=True)
    subprocess.run([str(binary)], check=True)
