// SPDX-License-Identifier: Apache-2.0

#include "btop.hpp"
#include "btop_tools.hpp"

#include <iterator>
#include <ranges>
#include <string_view>
#include <vector>

auto main(int argc, const char* argv[]) -> int {
	return btop_main(Tools::to_vector<std::string_view>(std::views::counted(std::next(argv), argc - 1)));
}
