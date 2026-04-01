#define DUCKDB_EXTENSION_MAIN

#include "decimal_arithmetic_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/function/aggregate_function.hpp"
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
// Execute functions — templated on input and result physical types.
//
// Intermediate arithmetic always happens in hugeint space to avoid overflow
// regardless of how small the inputs are. The final quotient is then cast to
// RESULT_TYPE, which matches the physical type of the result vector so that
// smaller result precisions use cheaper INT32/INT64 storage.
//===--------------------------------------------------------------------===//

template <class INPUT_TYPE, class RESULT_TYPE>
static void DecimalDivExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<DecimalDivBindData>();

	hugeint_t scale_factor = Hugeint::POWERS_OF_TEN[bind_data.scale_exp];

	BinaryExecutor::Execute<INPUT_TYPE, INPUT_TYPE, RESULT_TYPE>(
	    args.data[0], args.data[1], result, args.size(), [&](INPUT_TYPE a, INPUT_TYPE b) -> RESULT_TYPE {
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

		    hugeint_t final_val = negative ? -q : q;
		    RESULT_TYPE out;
		    if (!Hugeint::TryCast(final_val, out)) {
			    throw OutOfRangeException("decimal_div: result out of range for result type");
		    }
		    return out;
	    });
}

// Select the execute function for a given (input physical type, result physical type) pair.
template <class INPUT_TYPE>
static scalar_function_t GetDecimalDivExecuteFunction(PhysicalType result_physical) {
	switch (result_physical) {
	case PhysicalType::INT16:
		return DecimalDivExecute<INPUT_TYPE, int16_t>;
	case PhysicalType::INT32:
		return DecimalDivExecute<INPUT_TYPE, int32_t>;
	case PhysicalType::INT64:
		return DecimalDivExecute<INPUT_TYPE, int64_t>;
	default:
		return DecimalDivExecute<INPUT_TYPE, hugeint_t>;
	}
}

//===--------------------------------------------------------------------===//
// Bind function
//===--------------------------------------------------------------------===//

// Result precision and scale for e1 / e2 (SQL Server semantics):
//   result_scale     = max(6, s1 + p2 + 1)
//   result_precision = p1 - s1 + s2 + result_scale
// Both are capped at Decimal::MAX_WIDTH_DECIMAL (38).
static unique_ptr<FunctionData> DecimalDivBind(ClientContext &context, ScalarFunction &bound_function,
                                               vector<unique_ptr<Expression>> &arguments) {
	uint8_t p1, s1, p2, s2;
	if (!arguments[0]->return_type.GetDecimalProperties(p1, s1) ||
	    !arguments[1]->return_type.GetDecimalProperties(p2, s2)) {
		throw InvalidInputException("decimal_div: both arguments must be DECIMAL");
	}

	uint8_t result_scale = MaxValue<uint8_t>(6, s1 + p2 + 1);
	uint8_t result_precision = p1 - s1 + s2 + result_scale;

	// Cap to the maximum representable decimal precision (38). When the
	// formula yields a precision above 38 we reduce the scale by the same
	// amount so the integer part is preserved, then floor scale at 6.
	if (result_precision > Decimal::MAX_WIDTH_DECIMAL) {
		uint8_t excess = result_precision - Decimal::MAX_WIDTH_DECIMAL;

		// Guard against uint8_t underflow: excess can be larger than
		// result_scale (e.g. p1=38,s1=0,s2=5 gives excess=26, scale=21).
		// In that case clamp to 0 before applying the minimum-scale floor.
		if (excess <= result_scale) {
			result_scale -= excess;
		} else {
			result_scale = 0;
		}
		result_scale = MaxValue<uint8_t>(6, result_scale);
		result_precision = Decimal::MAX_WIDTH_DECIMAL;
	}

	// Determine the result physical type from result_precision so the result
	// vector uses the smallest sufficient storage type.
	PhysicalType result_physical;
	if (result_precision <= Decimal::MAX_WIDTH_INT16) {
		result_physical = PhysicalType::INT16;
	} else if (result_precision <= Decimal::MAX_WIDTH_INT32) {
		result_physical = PhysicalType::INT32;
	} else if (result_precision <= Decimal::MAX_WIDTH_INT64) {
		result_physical = PhysicalType::INT64;
	} else {
		result_physical = PhysicalType::INT128;
	}

	bound_function.return_type = LogicalType::DECIMAL(result_precision, result_scale);

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
		bound_function.function = GetDecimalDivExecuteFunction<int16_t>(result_physical);
		break;
	case PhysicalType::INT32:
		bound_function.arguments[0] = LogicalType::DECIMAL(9, s1);
		bound_function.arguments[1] = LogicalType::DECIMAL(9, s2);
		bound_function.function = GetDecimalDivExecuteFunction<int32_t>(result_physical);
		break;
	case PhysicalType::INT64:
		bound_function.arguments[0] = LogicalType::DECIMAL(18, s1);
		bound_function.arguments[1] = LogicalType::DECIMAL(18, s2);
		bound_function.function = GetDecimalDivExecuteFunction<int64_t>(result_physical);
		break;
	case PhysicalType::INT128:
		bound_function.arguments[0] = LogicalType::DECIMAL(38, s1);
		bound_function.arguments[1] = LogicalType::DECIMAL(38, s2);
		bound_function.function = GetDecimalDivExecuteFunction<hugeint_t>(result_physical);
		break;
	default:
		throw InternalException("decimal_div: unexpected physical type");
	}

	uint8_t scale_exp = s2 + result_scale - s1;
	return make_uniq<DecimalDivBindData>(scale_exp);
}

//===--------------------------------------------------------------------===//
// decimal_avg — aggregate
//===--------------------------------------------------------------------===//

// State: running scaled-integer sum and a non-NULL row count.
// count == 0 is the empty-group sentinel (no separate boolean needed).
struct DecimalAvgState {
	hugeint_t sum;  // accumulated integer value at the input's scale
	uint64_t count; // number of non-NULL rows accumulated so far
};

// Operation struct templated on the input physical type.
// All arithmetic is done in hugeint space regardless of input width,
// so even INT16 inputs accumulate without overflow until HUGEINT is exhausted.
template <class INPUT_TYPE>
struct DecimalAvgOperation {
	// Tell the executor to skip NULL inputs automatically.
	static bool IgnoreNull() {
		return true;
	}

	static void Initialize(DecimalAvgState &state) {
		state.sum = hugeint_t(0);
		state.count = 0;
	}

	// Called once per non-NULL input row.
	// Must be a function template: the executor calls it as
	// OP::template Operation<INPUT_TYPE, STATE, OP>(...).
	template <class INPUT, class STATE, class OP>
	static void Operation(STATE &state, const INPUT &input, AggregateUnaryInput &) {
		if (!Hugeint::TryAddInPlace(state.sum, hugeint_t(input))) {
			throw OutOfRangeException("decimal_avg: intermediate sum overflowed HUGEINT range");
		}
		state.count++;
	}

	// Called when an entire vector contains the same constant value.
	// Adds the value `count` times — equivalent to calling Operation in a loop
	// but avoids repeated function-call overhead for large constant batches.
	// Must be a function template: called as
	// OP::template ConstantOperation<INPUT_TYPE, STATE, OP>(...).
	template <class INPUT, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT &input, AggregateUnaryInput &unary_input, idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT, STATE, OP>(state, input, unary_input);
		}
	}

	// Merge two partial states (used in parallel / multi-threaded aggregation).
	// Must be a function template so the executor can call it as
	// OP::template Combine<STATE, OP>(...).
	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (!Hugeint::TryAddInPlace(target.sum, source.sum)) {
			throw OutOfRangeException("decimal_avg: intermediate sum overflowed HUGEINT range during combine");
		}
		target.count += source.count;
	}

	// Produce the final result.
	// The return type is always DECIMAL(38, s) which is physically INT128,
	// so the result target is hugeint_t.
	// Must be a function template: called as OP::template Finalize<RESULT, STATE>(...).
	template <class RESULT, class STATE>
	static void Finalize(STATE &state, RESULT &target, AggregateFinalizeData &fd) {
		if (state.count == 0) {
			fd.ReturnNull();
			return;
		}

		// Work in absolute-value space so the rounding logic is sign-agnostic,
		// mirroring the approach used in decimal_div.
		bool negative = state.sum.upper < 0;
		hugeint_t abssum = negative ? -state.sum : state.sum;

		// count always fits in uint64_t — it only ever counts rows.
		uint64_t r64;
		hugeint_t q = Hugeint::DivModPositive(abssum, state.count, r64);

		// Banker's rounding (round-half-to-even).
		// dist = count - r is how far the remainder is from the "other half".
		// Round up when r > dist (clearly past midpoint), or
		//             when r == dist (exactly halfway) and q is odd.
		uint64_t dist = state.count - r64;
		bool round_up = (dist < r64) | ((dist == r64) & static_cast<bool>(q.lower & 1));
		if (round_up) {
			q = q + hugeint_t(1);
		}

		target = negative ? -q : q;
	}
};

// Build the correctly-typed UnaryAggregate for a given input physical type.
// The result is always DECIMAL(38, scale) = INT128-backed.
template <class INPUT_TYPE>
static AggregateFunction MakeDecimalAvgFunction(const LogicalType &input_type, const LogicalType &return_type) {
	return AggregateFunction::UnaryAggregate<DecimalAvgState, INPUT_TYPE, hugeint_t, DecimalAvgOperation<INPUT_TYPE>>(
	    input_type, return_type);
}

// Bind: replace the placeholder function with a correctly-typed one for the
// actual input DECIMAL. No FunctionData is returned — the replaced function
// carries no bind callback, preventing any recursion.
static unique_ptr<FunctionData> BindDecimalAvg(ClientContext &, AggregateFunction &function,
                                               vector<unique_ptr<Expression>> &arguments) {
	auto &input_type = arguments[0]->return_type;

	// During overload resolution DuckDB may probe bind before types are fully
	// resolved. Return nullptr for UNKNOWN so DuckDB retries after resolution.
	if (input_type.id() == LogicalTypeId::UNKNOWN) {
		return nullptr;
	}
	// Mirror decimal_div: give a clean error instead of crashing in GetScale.
	if (input_type.id() != LogicalTypeId::DECIMAL) {
		throw InvalidInputException("decimal_avg requires a DECIMAL argument, got %s — "
		                            "cast the input explicitly, e.g. col::DECIMAL(18,2)",
		                            input_type.ToString());
	}

	uint8_t scale = DecimalType::GetScale(input_type);
	auto return_type = LogicalType::DECIMAL(Decimal::MAX_WIDTH_DECIMAL, scale);

	switch (input_type.InternalType()) {
	case PhysicalType::INT16:
		function = MakeDecimalAvgFunction<int16_t>(LogicalType::DECIMAL(Decimal::MAX_WIDTH_INT16, scale), return_type);
		break;
	case PhysicalType::INT32:
		function = MakeDecimalAvgFunction<int32_t>(LogicalType::DECIMAL(Decimal::MAX_WIDTH_INT32, scale), return_type);
		break;
	case PhysicalType::INT64:
		function = MakeDecimalAvgFunction<int64_t>(LogicalType::DECIMAL(Decimal::MAX_WIDTH_INT64, scale), return_type);
		break;
	case PhysicalType::INT128:
		function =
		    MakeDecimalAvgFunction<hugeint_t>(LogicalType::DECIMAL(Decimal::MAX_WIDTH_DECIMAL, scale), return_type);
		break;
	default:
		throw InternalException("decimal_avg: unexpected physical type");
	}
	function.name = "decimal_avg";
	return nullptr;
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
static void LoadInternal(ExtensionLoader &loader) {
	auto any_decimal = LogicalType(LogicalTypeId::DECIMAL);

	// decimal_div — scalar division with banker's rounding.
	// hugeint_t is the default execute; bind always overrides before execution.
	ScalarFunction decimal_div_func("decimal_div", {any_decimal, any_decimal}, any_decimal,
	                                DecimalDivExecute<hugeint_t, hugeint_t>, DecimalDivBind);
	loader.RegisterFunction(decimal_div_func);

	// decimal_avg — aggregate average with banker's rounding.
	// hugeint_t is used as the placeholder type for all callbacks; BindDecimalAvg
	// replaces the whole function with the correctly-typed one before any rows
	// are processed, so these placeholders are never actually called.
	AggregateFunction decimal_avg_func(
	    "decimal_avg", {any_decimal}, any_decimal, AggregateFunction::StateSize<DecimalAvgState>,
	    AggregateFunction::StateInitialize<DecimalAvgState, DecimalAvgOperation<hugeint_t>>,
	    AggregateFunction::UnaryScatterUpdate<DecimalAvgState, hugeint_t, DecimalAvgOperation<hugeint_t>>,
	    AggregateFunction::StateCombine<DecimalAvgState, DecimalAvgOperation<hugeint_t>>,
	    AggregateFunction::StateFinalize<DecimalAvgState, hugeint_t, DecimalAvgOperation<hugeint_t>>,
	    nullptr, // simple_update
	    BindDecimalAvg);
	loader.RegisterFunction(decimal_avg_func);
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
