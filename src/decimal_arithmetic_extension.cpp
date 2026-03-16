#define DUCKDB_EXTENSION_MAIN

#include "decimal_arithmetic_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Bind data
//===--------------------------------------------------------------------===//

struct DecimalDivBindData : public FunctionData {
	// Exponent for the scale factor applied to the numerator before dividing:
	//   result_int = (a_int * 10^scale_exp) / b_int
	// where scale_exp = s2 + result_scale - s1
	uint8_t scale_exp;

	explicit DecimalDivBindData(uint8_t scale_exp) : scale_exp(scale_exp) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DecimalDivBindData>(scale_exp);
	}

	bool Equals(const FunctionData &other) const override {
		return scale_exp == other.Cast<DecimalDivBindData>().scale_exp;
	}
};

//===--------------------------------------------------------------------===//
// Execute functions — one per physical type
//
// Each reads both inputs as INPUT_TYPE (same physical representation) and
// computes in hugeint space to avoid overflow, writing a hugeint result into
// the INT128 result vector (DECIMAL(38, result_scale)).
//
// Having a distinct function per physical type lets the bind function wire up
// the right one so constant-folding sees consistent types end-to-end.
//===--------------------------------------------------------------------===//

template <class INPUT_TYPE>
static void DecimalDivExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<DecimalDivBindData>();

	hugeint_t scale_factor = Hugeint::POWERS_OF_TEN[bind_data.scale_exp];

	BinaryExecutor::Execute<INPUT_TYPE, INPUT_TYPE, hugeint_t>(
	    args.data[0], args.data[1], result, args.size(), [&](INPUT_TYPE a, INPUT_TYPE b) -> hugeint_t {
		    if (b == INPUT_TYPE(0)) {
			    throw InvalidInputException("decimal_div: division by zero");
		    }

		    hugeint_t numerator = hugeint_t(a) * scale_factor;
		    hugeint_t divisor = hugeint_t(b);

		    // Work with absolute values so the rounding logic is sign-agnostic.
		    bool negative = (numerator.upper < 0) != (divisor.upper < 0);
		    hugeint_t abs_num = numerator.upper < 0 ? -numerator : numerator;
		    hugeint_t abs_div = divisor.upper < 0 ? -divisor : divisor;

		    // Single DivMod call instead of separate / then % — the bit-shift
		    // loop in Hugeint::DivMod is otherwise executed twice.
		    // Use the cheaper DivModPositive when the divisor fits in uint64_t,
		    // which is the common case for typical decimal values.
		    hugeint_t q, r;
		    if (abs_div.upper == 0) {
			    uint64_t r64;
			    q = Hugeint::DivModPositive(abs_num, abs_div.lower, r64);
			    r.upper = 0;
			    r.lower = r64;
		    } else {
			    q = Hugeint::DivMod(abs_num, abs_div, r);
		    }

		    // Banker's rounding (round-half-to-even), collapsed to one branch.
		    // Compare r against (abs_div - r) to avoid computing 2*r.
		    hugeint_t dist = abs_div - r;
		    bool round_up = (dist < r) | ((dist == r) & (bool)(q.lower & 1));
		    if (round_up) {
			    q = q + hugeint_t(1);
		    }

		    return negative ? -q : q;
	    });
}

//===--------------------------------------------------------------------===//
// Bind function
//===--------------------------------------------------------------------===//

// Result precision and scale for e1 / e2:
//   result_scale     = max(6, s1 + p2 + 1)
//   result_precision = p1 - s1 + s2 + result_scale   (always pinned to 38 so
//                      the result vector is always INT128, matching the hugeint
//                      return type of every DecimalDivExecute instantiation)
static unique_ptr<FunctionData> DecimalDivBind(ClientContext &context, ScalarFunction &bound_function,
                                               vector<unique_ptr<Expression>> &arguments) {
	uint8_t p1, s1, p2, s2;

	if (!arguments[0]->return_type.GetDecimalProperties(p1, s1) ||
	    !arguments[1]->return_type.GetDecimalProperties(p2, s2)) {
		throw InvalidInputException("decimal_div: both arguments must be DECIMAL");
	}

	uint8_t result_scale = MaxValue<uint8_t>(6, s1 + p2 + 1);

	// Always INT128 result so every execute instantiation shares one result type.
	bound_function.return_type = LogicalType::DECIMAL(38, result_scale);

	// Normalise both operands to the wider physical type so the execute
	// function sees consistent physical types.  The max-precision value for
	// each physical tier is used as the representative precision; scale is
	// preserved so no numeric value is lost during the cast.
	auto lhs_physical = arguments[0]->return_type.InternalType();
	auto rhs_physical = arguments[1]->return_type.InternalType();
	auto wider = MaxValue<PhysicalType>(lhs_physical, rhs_physical);

	switch (wider) {
	case PhysicalType::INT16:
		bound_function.arguments[0] = LogicalType::DECIMAL(4, s1);
		bound_function.arguments[1] = LogicalType::DECIMAL(4, s2);
		bound_function.function = DecimalDivExecute<int16_t>;
		break;
	case PhysicalType::INT32:
		bound_function.arguments[0] = LogicalType::DECIMAL(9, s1);
		bound_function.arguments[1] = LogicalType::DECIMAL(9, s2);
		bound_function.function = DecimalDivExecute<int32_t>;
		break;
	case PhysicalType::INT64:
		bound_function.arguments[0] = LogicalType::DECIMAL(18, s1);
		bound_function.arguments[1] = LogicalType::DECIMAL(18, s2);
		bound_function.function = DecimalDivExecute<int64_t>;
		break;
	case PhysicalType::INT128:
		bound_function.arguments[0] = LogicalType::DECIMAL(38, s1);
		bound_function.arguments[1] = LogicalType::DECIMAL(38, s2);
		bound_function.function = DecimalDivExecute<hugeint_t>;
		break;
	default:
		throw InternalException("decimal_div: unexpected physical type");
	}

	uint8_t scale_exp = s2 + result_scale - s1;
	return make_uniq<DecimalDivBindData>(scale_exp);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
static void LoadInternal(ExtensionLoader &loader) {
	auto any_decimal = LogicalType(LogicalTypeId::DECIMAL);
	// hugeint is the default; bind always overrides it before execution.
	ScalarFunction decimal_div_func("decimal_div", {any_decimal, any_decimal}, any_decimal,
	                                DecimalDivExecute<hugeint_t>, DecimalDivBind);
	loader.RegisterFunction(decimal_div_func);
}

void DecimalArithmeticExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string DecimalArithmeticExtension::Name() {
	return "decimal_arithmetic";
}

std::string DecimalArithmeticExtension::Version() const {
#ifdef EXT_VERSION_DECIMAL_ARITHMETIC
	return EXT_VERSION_DECIMAL_ARITHMETIC;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(decimal_arithmetic, loader) {
	duckdb::LoadInternal(loader);
}
}