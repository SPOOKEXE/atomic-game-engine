#include <iostream>
#include <pxcimport/Runner.hpp>

int main(int argc, char **argv) {
	return pxcimport::Run(argc, argv, std::cout, std::cerr);
}
