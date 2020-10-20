/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
#include <libyul/optimiser/UnusedFunctionReturnParameterPruner.h>
#include <libyul/optimiser/UnusedFunctionsCommon.h>
#include <libyul/optimiser/NameCollector.h>
#include <libyul/optimiser/NameDisplacer.h>
#include <libyul/optimiser/ASTWalker.h>

#include <libyul/AsmDataForward.h>
#include <libyul/AsmData.h>
#include <libyul/Dialect.h>

#include <libsolutil/CommonData.h>
#include <variant>

using namespace std;
using namespace solidity::util;
using namespace solidity::yul;
using namespace solidity::langutil;
using namespace solidity::yul::unusedFunctionsCommon;

/**
 * Step 1 of UnusedFunctionReturnParameterPruner: Find functions whose return parameters are not
 * used in the code, i.e.,
 *
 * Look at all VariableDeclaration `let x_1, ..., x_i, ..., x_n := f(y_1, ..., y_m)` and check if
 * `x_i` is unused. If all function calls to `f` has its i-th return parameter unused, we will mark
 * its i-th index so that it can be removed in later steps.
 */
class FindFunctionsWithUnusedReturns: public ASTWalker
{
public:
	explicit FindFunctionsWithUnusedReturns(
		Dialect const& _dialect,
		Block const& _block,
		map<YulString, size_t> const& _references
	):
		m_references(_references),
		m_dialect(_dialect)
	{
		(*this)(_block);
	}
	using ASTWalker::operator();
	void operator()(VariableDeclaration const& _v) override;

	map<YulString, vector<bool>> functions();

private:
	/// Function name and a boolean mask, where `false` at index `i` indicates that the function
	/// return-parameter at index `i` in `FunctionDefinition::parameter` is unused at every function
	/// call-site.
	map<YulString, vector<bool>> m_callSiteMasks;
	/// A count of all references to all Declarations.
	map<YulString, size_t> const& m_references;
	Dialect const& m_dialect;
};

/// Find functions whose return parameters are unused. Assuming that the code is in SSA form and
/// that ForLoopConditionIntoBody, ExpressionSplitter were run, all FunctionCalls with at least one
/// return value will have the form `let x_1, ..., x_n := f(y_1, ..., y_m)`
void FindFunctionsWithUnusedReturns::operator()(VariableDeclaration const& _v)
{
	if (holds_alternative<FunctionCall>(*_v.value))
	{
		YulString const& name = (get<FunctionCall>(*_v.value)).functionName.name;

		if (m_dialect.builtin(name))
			return;

		if (!m_callSiteMasks.count(name))
			m_callSiteMasks[name].resize(_v.variables.size(), false);

		for (size_t i = 0; i < _v.variables.size(); ++i)
			m_callSiteMasks.at(name)[i] =
				m_callSiteMasks.at(name)[i] || m_references.count(_v.variables[i].name);
	}
}

/// Returns a map with function name as key and vector of bools such that if the index `i` of the
/// vector is false, then the function's `i`-th return parameter is unused at *all* callsites.
map<YulString, vector<bool>> FindFunctionsWithUnusedReturns::functions()
{
	map<YulString, vector<bool>> functions;

	for (auto const& [name, mask]: m_callSiteMasks)
		if (any_of(mask.begin(), mask.end(), [](bool _b) { return !_b; }))
			functions[name] = mask;

	return functions;
}

void UnusedFunctionReturnParameterPruner::run(OptimiserStepContext& _context, Block& _ast)
{
	map<YulString, size_t> references = ReferencesCounter::countReferences(_ast);
	FindFunctionsWithUnusedReturns find{_context.dialect, _ast, references};

	map<YulString, vector<bool>> functions = find.functions();

	set<YulString> simpleFunctions;
	for (auto const& s: _ast.statements)
		if (holds_alternative<FunctionDefinition>(s))
		{
			FunctionDefinition const& f = get<FunctionDefinition>(s);
			if (tooSimpleToBePruned(f))
				simpleFunctions.insert(f.name);
		}

	set<YulString> functionNamesToFree = util::keys(functions) - simpleFunctions;

	// Step 2 of UnusedFunctionReturnParameterPruner: Renames the function and replaces all references to
	// the function, say `f`, by its new name, say `f_1`.
	NameDisplacer replace{_context.dispenser, functionNamesToFree};
	replace(_ast);

	// Inverse-Map of the above translations. In the above example, this will store an element with
	// key `f_1` and value `f`.
	std::map<YulString, YulString> newToOriginalNames = invertMap(replace.translations());

	// Step 3 of UnusedFunctionReturnParameterPruner: introduce a new function in the block with body of
	// the old one. Replace the body of the old one with a function call to the new one with reduced
	// parameters.
	//
	// For example: introduce a new 'linking' function `f` with the same the body as `f_1`, but with
	// reduced return parameters, e.g., if `y` is unused at all callsites in the following
	// definition: `function f() -> y { y := 1 }`. We create a linking function `f_1` whose body
	// calls to `f`, i.e., `f_1(x) -> y { y := f() }`.
	//
	// Note that all parameter names of the linking function has to be renamed to avoid conflict.
	iterateReplacing(_ast.statements, [&](Statement& _s) -> optional<vector<Statement>> {
		if (holds_alternative<FunctionDefinition>(_s))
		{
			// The original function except that it has a new name (e.g., `f_1`)
			FunctionDefinition& originalFunction = get<FunctionDefinition>(_s);
			if (newToOriginalNames.count(originalFunction.name))
			{

				YulString linkingFunctionName = originalFunction.name;
				YulString originalFunctionName = newToOriginalNames.at(linkingFunctionName);
				pair<vector<bool>, vector<bool>> used =	{
					vector<bool>(originalFunction.parameters.size(), true),
					functions.at(originalFunctionName)
				};

				FunctionDefinition linkingFunction = createLinkingFunction(
					originalFunction,
					used,
					originalFunctionName,
					linkingFunctionName,
					_context.dispenser
				);

				originalFunction.name = originalFunctionName;
				auto missingVariables = filter(
					originalFunction.returnVariables,
					applyMap(used.second, [](bool _b) {return !_b;})
				);
				originalFunction.returnVariables =
					filter(originalFunction.returnVariables, used.second);
				// Return variables that are pruned can still be used inside the function body.
				// Therefore, an extra `let missingVariables` needs to be added at the beginning of
				// the function body.
				originalFunction.body.statements.emplace(
					originalFunction.body.statements.begin(),
					VariableDeclaration{
						originalFunction.location,
						missingVariables,
						nullptr
					}
				);

				return make_vector<Statement>(move(originalFunction), move(linkingFunction));
			}
		}

		return nullopt;
	});

}
