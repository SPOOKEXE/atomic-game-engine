#include <engine/testing/Bench.hpp>

#include <iostream>

namespace {

	const engine::testing::SuiteDeclaration suiteAlpha{"fixture.alpha", __FILE__};
	const engine::testing::SuiteDeclaration suiteBeta{"fixture.beta", __FILE__};

	const engine::testing::BenchDeclaration alphaShared{"fixture.alpha", "shared", 1, [] {
															std::cout << "BODY_ALPHA_SHARED\n";
														}};
	const engine::testing::BenchDeclaration betaShared{"fixture.beta", "shared", 1, [] {
														   std::cout << "BODY_BETA_SHARED\n";
													   }};
	const engine::testing::BenchDeclaration alphaExtended{"fixture.alpha", "shared extended", 1, [] {
															  std::cout << "BODY_ALPHA_EXTENDED\n";
														  }};
}
