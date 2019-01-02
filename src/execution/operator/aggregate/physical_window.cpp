#include "execution/operator/aggregate/physical_window.hpp"

#include "common/types/chunk_collection.hpp"
#include "common/vector_operations/vector_operations.hpp"
#include "execution/expression_executor.hpp"
#include "parser/expression/aggregate_expression.hpp"
#include "parser/expression/columnref_expression.hpp"
#include "parser/expression/constant_expression.hpp"
#include "parser/expression/window_expression.hpp"

using namespace duckdb;
using namespace std;

// this implements a sorted window functions variant
PhysicalWindow::PhysicalWindow(LogicalOperator &op, vector<unique_ptr<Expression>> select_list,
                               PhysicalOperatorType type)
    : PhysicalOperator(type, op.types), select_list(std::move(select_list)) {
}

static bool EqualsSubset(vector<Value> &a, vector<Value> &b, size_t start, size_t end) {
	assert(start <= end);
	for (size_t i = start; i < end; i++) {
		if (a[i] != b[i]) {
			return false;
		}
	}
	return true;
}

static size_t BinarySearchRightmost(ChunkCollection &input, vector<Value> row, size_t l, size_t r, size_t comp_cols) {
	if (comp_cols == 0) {
		return r - 1;
	}
	while (l < r) {
		size_t m = floor((l + r) / 2);
		bool less_than_equals = true;
		for (size_t i = 0; i < comp_cols; i++) {
			if (input.GetRow(m)[i] > row[i]) {
				less_than_equals = false;
				break;
			}
		}
		if (less_than_equals) {
			l = m + 1;
		} else {
			r = m;
		}
	}
	return l - 1;
}

static void MaterializeExpression(ClientContext &context, Expression *expr, ChunkCollection &input,
                                  ChunkCollection &output, bool scalar = false) {
	ChunkCollection boundary_start_collection;
	vector<TypeId> types = {expr->return_type};
	for (size_t i = 0; i < input.chunks.size(); i++) {
		DataChunk chunk;
		chunk.Initialize(types);
		ExpressionExecutor executor(*input.chunks[i], context);
		executor.ExecuteExpression(expr, chunk.data[0]);

		chunk.Verify();
		output.Append(chunk);

		if (scalar) {
			break;
		}
	}
}

static void SortCollectionForWindow(ClientContext &context, WindowExpression *wexpr, ChunkCollection &input,
                                    ChunkCollection &output) {
	vector<TypeId> sort_types;
	vector<Expression *> exprs;
	OrderByDescription odesc;

	// we sort by both 1) partition by expression list and 2) order by expressions
	for (size_t prt_idx = 0; prt_idx < wexpr->partitions.size(); prt_idx++) {
		auto &pexpr = wexpr->partitions[prt_idx];
		sort_types.push_back(pexpr->return_type);
		exprs.push_back(pexpr.get());
		odesc.orders.push_back(OrderByNode(OrderType::ASCENDING, make_unique_base<Expression, ColumnRefExpression>(
		                                                             pexpr->return_type, exprs.size() - 1)));
	}

	for (size_t ord_idx = 0; ord_idx < wexpr->ordering.orders.size(); ord_idx++) {
		auto &oexpr = wexpr->ordering.orders[ord_idx].expression;
		sort_types.push_back(oexpr->return_type);
		exprs.push_back(oexpr.get());
		odesc.orders.push_back(
		    OrderByNode(wexpr->ordering.orders[ord_idx].type,
		                make_unique_base<Expression, ColumnRefExpression>(oexpr->return_type, exprs.size() - 1)));
	}

	assert(sort_types.size() > 0);

	// create a chunkcollection for the results of the expressions in the window definitions
	for (size_t i = 0; i < input.chunks.size(); i++) {
		DataChunk sort_chunk;
		sort_chunk.Initialize(sort_types);

		ExpressionExecutor executor(*input.chunks[i], context);
		executor.Execute(sort_chunk, [&](size_t i) { return exprs[i]; }, exprs.size());
		sort_chunk.Verify();
		output.Append(sort_chunk);
	}

	assert(input.count == output.count);

	auto sorted_vector = unique_ptr<uint64_t[]>(new uint64_t[input.count]);
	output.Sort(odesc, sorted_vector.get());

	input.Reorder(sorted_vector.get());
	output.Reorder(sorted_vector.get());
}

struct WindowBoundariesState {
	size_t partition_start = 0;
	size_t partition_end = 0;
	size_t peer_start = 0;
	size_t peer_end = 0;
	ssize_t window_start = -1;
	ssize_t window_end = -1;
	bool is_same_partition = false;
	bool is_peer = false;
	vector<Value> row_prev;
};

static void UpdateWindowBoundaries(WindowExpression *wexpr, ChunkCollection &input, size_t row_idx,
                                   ChunkCollection &boundary_start_collection, ChunkCollection &boundary_end_collection,
                                   WindowBoundariesState &bounds) {
	vector<Value> row_cur = input.GetRow(row_idx);
	size_t sort_col_count = wexpr->partitions.size() + wexpr->ordering.orders.size();

	// determine partition and peer group boundaries to ultimately figure out window size
	bounds.is_same_partition = EqualsSubset(bounds.row_prev, row_cur, 0, wexpr->partitions.size());
	bounds.is_peer =
	    bounds.is_same_partition && EqualsSubset(bounds.row_prev, row_cur, wexpr->partitions.size(), sort_col_count);
	bounds.row_prev = row_cur;

	// when the partition changes, recompute the boundaries
	if (!bounds.is_same_partition || row_idx == 0) { // special case for first row, need to init
		bounds.partition_start = row_idx;
		bounds.peer_start = row_idx;

		// find end of partition
		bounds.partition_end =
		    BinarySearchRightmost(input, row_cur, bounds.partition_start, input.count, wexpr->partitions.size()) + 1;

	} else if (!bounds.is_peer) {
		bounds.peer_start = row_idx;
	}

	if (wexpr->end == WindowBoundary::CURRENT_ROW_RANGE) {
		bounds.peer_end = BinarySearchRightmost(input, row_cur, row_idx, bounds.partition_end, sort_col_count) + 1;
	}

	// determine window boundaries depending on the type of expression
	bounds.window_start = -1;
	bounds.window_end = -1;

	switch (wexpr->start) {
	case WindowBoundary::UNBOUNDED_PRECEDING:
		bounds.window_start = bounds.partition_start;
		break;
	case WindowBoundary::CURRENT_ROW_ROWS:
		bounds.window_start = row_idx;
		break;
	case WindowBoundary::CURRENT_ROW_RANGE:
		bounds.window_start = bounds.peer_start;
		break;
	case WindowBoundary::UNBOUNDED_FOLLOWING:
		assert(0); // disallowed
		break;
	case WindowBoundary::EXPR_PRECEDING: {
		assert(boundary_start_collection.column_count() > 0);
		bounds.window_start =
		    (ssize_t)row_idx -
		    boundary_start_collection.GetValue(0, wexpr->start_expr->IsScalar() ? 0 : row_idx).GetNumericValue();
		break;
	}
	case WindowBoundary::EXPR_FOLLOWING: {
		assert(boundary_start_collection.column_count() > 0);
		bounds.window_start =
		    row_idx +
		    boundary_start_collection.GetValue(0, wexpr->start_expr->IsScalar() ? 0 : row_idx).GetNumericValue();
		break;
	}

	default:
		throw NotImplementedException("Unsupported boundary");
	}

	switch (wexpr->end) {
	case WindowBoundary::UNBOUNDED_PRECEDING:
		assert(0); // disallowed
		break;
	case WindowBoundary::CURRENT_ROW_ROWS:
		bounds.window_end = row_idx + 1;
		break;
	case WindowBoundary::CURRENT_ROW_RANGE:
		bounds.window_end = bounds.peer_end;
		break;
	case WindowBoundary::UNBOUNDED_FOLLOWING:
		bounds.window_end = bounds.partition_end;
		break;
	case WindowBoundary::EXPR_PRECEDING:
		assert(boundary_end_collection.column_count() > 0);
		bounds.window_end =
		    (ssize_t)row_idx -
		    boundary_end_collection.GetValue(0, wexpr->end_expr->IsScalar() ? 0 : row_idx).GetNumericValue() + 1;
		break;
	case WindowBoundary::EXPR_FOLLOWING:
		assert(boundary_end_collection.column_count() > 0);
		bounds.window_end =
		    row_idx + boundary_end_collection.GetValue(0, wexpr->end_expr->IsScalar() ? 0 : row_idx).GetNumericValue() +
		    1;

		break;
	default:
		throw NotImplementedException("Unsupported boundary");
	}

	// clamp windows to partitions if they should exceed
	if (bounds.window_start < (ssize_t)bounds.partition_start) {
		bounds.window_start = bounds.partition_start;
	}
	if (bounds.window_end > bounds.partition_end) {
		bounds.window_end = bounds.partition_end;
	}

	if (bounds.window_start < 0 || bounds.window_end < 0) {
		throw Exception("Failed to compute window boundaries");
	}
}

static void ComputeWindowExpression(ClientContext &context, WindowExpression *wexpr, ChunkCollection &input,
                                    ChunkCollection &output, size_t output_idx) {

	// TODO: if we have no sort nor order by we don't have to sort and the window is everything
	ChunkCollection sort_collection;
	size_t sort_col_count = wexpr->partitions.size() + wexpr->ordering.orders.size();
	if (sort_col_count > 0) {
		SortCollectionForWindow(context, wexpr, input, sort_collection);
	}

	// evaluate inner expressions of window functions, could be more complex
	ChunkCollection payload_collection;
	if (wexpr->children.size() > 0) {
		// TODO: child[0] may be a scalar, don't need to materialize the whole collection then
		MaterializeExpression(context, wexpr->children[0].get(), input, payload_collection);
	}

	// evaluate boundaries if present.
	ChunkCollection boundary_start_collection;
	if (wexpr->start_expr &&
	    (wexpr->start == WindowBoundary::EXPR_PRECEDING || wexpr->start == WindowBoundary::EXPR_FOLLOWING)) {
		MaterializeExpression(context, wexpr->start_expr.get(), input, boundary_start_collection,
		                      wexpr->start_expr->IsScalar());
	}
	ChunkCollection boundary_end_collection;
	if (wexpr->end_expr &&
	    (wexpr->end == WindowBoundary::EXPR_PRECEDING || wexpr->end == WindowBoundary::EXPR_FOLLOWING)) {
		MaterializeExpression(context, wexpr->end_expr.get(), input, boundary_end_collection,
		                      wexpr->end_expr->IsScalar());
	}

	WindowBoundariesState bounds;

	// build a segment tree for frame-adhering aggregates
	// see http://www.vldb.org/pvldb/vol8/p1058-leis.pdf
	unique_ptr<WindowSegmentTree> segment_tree = nullptr;
	switch (wexpr->type) {
	case ExpressionType::WINDOW_SUM:
	case ExpressionType::WINDOW_MIN:
	case ExpressionType::WINDOW_MAX:
	case ExpressionType::WINDOW_AVG:
		segment_tree = make_unique<WindowSegmentTree>(wexpr->type, wexpr->return_type, 16);
		segment_tree->Construct(payload_collection);
		break;
	default:
		break;
		// nothing
	}

	size_t dense_rank, rank_equal, rank;
	bounds.row_prev = sort_collection.GetRow(0);

	// this is the main loop, go through all sorted rows and compute window function result
	for (size_t row_idx = 0; row_idx < input.count; row_idx++) {

		UpdateWindowBoundaries(wexpr, sort_collection, row_idx, boundary_start_collection, boundary_end_collection,
		                       bounds);

		if (!bounds.is_same_partition || row_idx == 0) { // special case for first row, need to init
			dense_rank = 1;
			rank = 1;
			rank_equal = 0;
		} else if (!bounds.is_peer) {
			dense_rank++;
			rank += rank_equal;
			rank_equal = 0;
		}

		auto res = Value();

		// if no values are read for window, result is NULL
		if (bounds.window_start >= bounds.window_end) {
			output.SetValue(output_idx, row_idx, res);
			continue;
		}

		switch (wexpr->type) {
		case ExpressionType::WINDOW_SUM:
		case ExpressionType::WINDOW_MIN:
		case ExpressionType::WINDOW_MAX:
		case ExpressionType::WINDOW_AVG:
			assert(segment_tree);
			res = segment_tree->Compute(bounds.window_start, bounds.window_end);
			break;

		case ExpressionType::WINDOW_COUNT_STAR: {
			res = Value::Numeric(wexpr->return_type, bounds.window_end - bounds.window_start);
			break;
		}
		case ExpressionType::WINDOW_ROW_NUMBER: {
			res = Value::Numeric(wexpr->return_type, row_idx - bounds.window_start + 1);
			break;
		}
		case ExpressionType::WINDOW_RANK_DENSE: {
			res = Value::Numeric(wexpr->return_type, dense_rank);
			break;
		}
		case ExpressionType::WINDOW_RANK: {
			res = Value::Numeric(wexpr->return_type, rank);
			rank_equal++;
			break;
		}
		case ExpressionType::WINDOW_FIRST_VALUE: {
			res = payload_collection.GetValue(0, bounds.window_start);
			break;
		}
		case ExpressionType::WINDOW_LAST_VALUE: {
			res = payload_collection.GetValue(0, bounds.window_end - 1);
			break;
		}
		default:
			throw NotImplementedException("Window aggregate type %s", ExpressionTypeToString(wexpr->type).c_str());
		}
		output.SetValue(output_idx, row_idx, res);
	}
}

void PhysicalWindow::_GetChunk(ClientContext &context, DataChunk &chunk, PhysicalOperatorState *state_) {
	auto state = reinterpret_cast<PhysicalWindowOperatorState *>(state_);
	ChunkCollection &big_data = state->tuples;
	ChunkCollection &window_results = state->window_results;

	// this is a blocking operator, so compute complete result on first invocation
	if (state->position == 0) {
		do {
			children[0]->GetChunk(context, state->child_chunk, state->child_state.get());
			big_data.Append(state->child_chunk);
		} while (state->child_chunk.size() != 0);

		if (big_data.count == 0) {
			return;
		}

		vector<TypeId> window_types;
		for (size_t expr_idx = 0; expr_idx < select_list.size(); expr_idx++) {
			window_types.push_back(select_list[expr_idx]->return_type);
		}

		for (size_t i = 0; i < big_data.chunks.size(); i++) {
			DataChunk window_chunk;
			window_chunk.Initialize(window_types);
			for (size_t col_idx = 0; col_idx < window_chunk.column_count; col_idx++) {
				window_chunk.data[col_idx].count = big_data.chunks[i]->size();
			}
			window_chunk.Verify();
			window_results.Append(window_chunk);
		}

		assert(window_results.column_count() == select_list.size());
		size_t window_output_idx = 0;
		// we can have multiple window functions
		for (size_t expr_idx = 0; expr_idx < select_list.size(); expr_idx++) {
			assert(select_list[expr_idx]->GetExpressionClass() == ExpressionClass::WINDOW);
			// sort by partition and order clause in window def
			auto wexpr = reinterpret_cast<WindowExpression *>(select_list[expr_idx].get());
			ComputeWindowExpression(context, wexpr, big_data, window_results, window_output_idx++);
		}
	}

	if (state->position >= big_data.count) {
		return;
	}

	// just return what was computed before, appending the result cols of the window expressions at the end
	auto &proj_ch = big_data.GetChunk(state->position);
	auto &wind_ch = window_results.GetChunk(state->position);

	size_t out_idx = 0;
	for (size_t col_idx = 0; col_idx < proj_ch.column_count; col_idx++) {
		chunk.data[out_idx++].Reference(proj_ch.data[col_idx]);
	}
	for (size_t col_idx = 0; col_idx < wind_ch.column_count; col_idx++) {
		chunk.data[out_idx++].Reference(wind_ch.data[col_idx]);
	}
	state->position += STANDARD_VECTOR_SIZE;
}

unique_ptr<PhysicalOperatorState> PhysicalWindow::GetOperatorState(ExpressionExecutor *parent) {
	return make_unique<PhysicalWindowOperatorState>(children[0].get(), parent);
}

void WindowSegmentTree::AggregateInit() {
	switch (window_type) {
	case ExpressionType::WINDOW_SUM:
	case ExpressionType::WINDOW_AVG:
		aggregate = Value::Numeric(payload_type, 0);
		break;
	case ExpressionType::WINDOW_MIN:
		aggregate = Value::MaximumValue(payload_type);
		break;
	case ExpressionType::WINDOW_MAX:
		aggregate = Value::MinimumValue(payload_type);
		break;
	default:
		throw NotImplementedException("Window Type");
	}
	n_aggregated = 0;
}

void WindowSegmentTree::AggregateAccum(Value val) {
	switch (window_type) {
	case ExpressionType::WINDOW_SUM:
	case ExpressionType::WINDOW_AVG:
		aggregate = aggregate + val;
		break;
	case ExpressionType::WINDOW_MIN:
		if (val < aggregate) {
			aggregate = val;
		}
		break;
	case ExpressionType::WINDOW_MAX:
		if (val > aggregate) {
			aggregate = val;
		}
		break;
	default:
		throw NotImplementedException("Window Type");
	}
	n_aggregated++;
}

Value WindowSegmentTree::AggegateFinal() {
	if (n_aggregated == 0) {
		return Value().CastAs(payload_type);
	}
	switch (window_type) {
	case ExpressionType::WINDOW_SUM:
	case ExpressionType::WINDOW_MIN:
	case ExpressionType::WINDOW_MAX:
		return aggregate;
	case ExpressionType::WINDOW_AVG:
		return aggregate / Value::Numeric(payload_type, n_aggregated);
	default:
		throw NotImplementedException("Window Type");
	}

	return aggregate;
}

void WindowSegmentTree::Construct(ChunkCollection &input) {
	assert(input.column_count() == 1);
	AggregateInit();
	input_ref = &input;
	// level 0 is data itself
	size_t level_size;
	while ((level_size = (levels.size() == 0 ? input_ref->count : levels[levels.size() - 1].size())) > 1) {
		vector<Value> nl;
		size_t fanout_count = 0;
		for (size_t pos = 0; pos < level_size; pos++) {
			AggregateAccum((levels.size() == 0 ? input_ref->GetValue(0, pos) : levels[levels.size() - 1][pos]));
			fanout_count++;
			if (fanout_count == fanout) {
				nl.push_back(AggegateFinal());
				AggregateInit();
				fanout_count = 0;
			}
		}
		if (fanout_count > 0) {
			nl.push_back(AggegateFinal());
		}
		levels.push_back(nl);
	}
}

void WindowSegmentTree::WindowSegmentValue(size_t l_idx, size_t begin, size_t end) {
	assert(begin <= end);
	for (size_t pos = begin; pos < end; pos++) {
		AggregateAccum(l_idx == 0 ? input_ref->GetValue(0, pos) : levels[l_idx - 1][pos]);
	}
}

Value WindowSegmentTree::Compute(size_t begin, size_t end) {
	assert(input_ref);
	AggregateInit();
	for (size_t l_idx = 0; l_idx < levels.size() + 1; l_idx++) {
		size_t parent_begin = begin / fanout;
		size_t parent_end = end / fanout;
		if (parent_begin == parent_end) {
			WindowSegmentValue(l_idx, begin, end);
			return AggegateFinal();
		}
		size_t group_begin = parent_begin * fanout;
		if (begin != group_begin) {
			WindowSegmentValue(l_idx, begin, group_begin + fanout);
			parent_begin++;
		}
		size_t group_end = parent_end * fanout;
		if (end != group_end) {
			WindowSegmentValue(l_idx, group_end, end);
		}
		begin = parent_begin;
		end = parent_end;
	}

	return AggegateFinal();
}