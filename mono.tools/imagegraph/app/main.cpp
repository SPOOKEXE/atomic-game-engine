#include <imagegraph_runner/Runner.hpp>
#include <iostream>

int main(int argc, char **argv) {
	return imagegraph_runner::Run(argc, argv, std::cout, std::cerr);
}
