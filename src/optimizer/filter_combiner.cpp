#include "duckdb/optimizer/filter_combiner.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {

using ExpressionValueInformation = FilterCombiner::ExpressionValueInformation;

ValueComparisonResult CompareValueInformation(ExpressionValueInformation &left, ExpressionValueInformation &right);

Expression *FilterCombiner::GetNode(Expression *expr) {
	auto entry = stored_expressions.find(expr);
	if (entry != stored_expressions.end()) {
		// expression already exists: return a reference to the stored expression
		return entry->second.get();
	}
	// expression does not exist yet: create a copy and store it
	auto copy = expr->Copy();
	auto pointer_copy = copy.get();
	D_ASSERT(stored_expressions.find(pointer_copy) == stored_expressions.end());
	stored_expressions.insert(make_pair(pointer_copy, move(copy)));
	return pointer_copy;
}

idx_t FilterCombiner::GetEquivalenceSet(Expression *expr) {
	D_ASSERT(stored_expressions.find(expr) != stored_expressions.end());
	D_ASSERT(stored_expressions.find(expr)->second.get() == expr);

	auto entry = equivalence_set_map.find(expr);
	if (entry == equivalence_set_map.end()) {
		idx_t index = set_index++;
		equivalence_set_map[expr] = index;
		equivalence_map[index].push_back(expr);
		constant_values.insert(make_pair(index, vector<ExpressionValueInformation>()));
		return index;
	} else {
		return entry->second;
	}
}

FilterResult FilterCombiner::AddConstantComparison(vector<ExpressionValueInformation> &info_list,
                                                   ExpressionValueInformation info) {
	for (idx_t i = 0; i < info_list.size(); i++) {
		auto comparison = CompareValueInformation(info_list[i], info);
		switch (comparison) {
		case ValueComparisonResult::PRUNE_LEFT:
			// prune the entry from the info list
			info_list.erase(info_list.begin() + i);
			i--;
			break;
		case ValueComparisonResult::PRUNE_RIGHT:
			// prune the current info
			return FilterResult::SUCCESS;
		case ValueComparisonResult::UNSATISFIABLE_CONDITION:
			// combination of filters is unsatisfiable: prune the entire branch
			return FilterResult::UNSATISFIABLE;
		default:
			// prune nothing, move to the next condition
			break;
		}
	}
	// finally add the entry to the list
	info_list.push_back(info);
	return FilterResult::SUCCESS;
}

FilterResult FilterCombiner::AddFilter(unique_ptr<Expression> expr) {
	// try to push the filter into the combiner
	auto result = AddFilter(expr.get());
	if (result == FilterResult::UNSUPPORTED) {
		// unsupported filter, push into remaining filters
		remaining_filters.push_back(move(expr));
		return FilterResult::SUCCESS;
	}
	return result;
}

void FilterCombiner::GenerateFilters(std::function<void(unique_ptr<Expression> filter)> callback) {
	// first loop over the remaining filters
	for (auto &filter : remaining_filters) {
		callback(move(filter));
	}
	remaining_filters.clear();
	// now loop over the equivalence sets
	for (auto &entry : equivalence_map) {
		auto equivalence_set = entry.first;
		auto &entries = entry.second;
		auto &constant_list = constant_values.find(equivalence_set)->second;
		// for each entry generate an equality expression comparing to each other
		for (idx_t i = 0; i < entries.size(); i++) {
			for (idx_t k = i + 1; k < entries.size(); k++) {
				auto comparison = make_unique<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL,
				                                                         entries[i]->Copy(), entries[k]->Copy());
				callback(move(comparison));
			}
			// for each entry also create a comparison with each constant
			int lower_index = -1, upper_index = -1;
			bool lower_inclusive, upper_inclusive;
			for (idx_t k = 0; k < constant_list.size(); k++) {
				auto &info = constant_list[k];
				if (info.comparison_type == ExpressionType::COMPARE_GREATERTHAN ||
				    info.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
					lower_index = k;
					lower_inclusive = info.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
				} else if (info.comparison_type == ExpressionType::COMPARE_LESSTHAN ||
				           info.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
					upper_index = k;
					upper_inclusive = info.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO;
				} else {
					auto constant = make_unique<BoundConstantExpression>(info.constant);
					auto comparison = make_unique<BoundComparisonExpression>(info.comparison_type, entries[i]->Copy(),
					                                                         move(constant));
					callback(move(comparison));
				}
			}
			if (lower_index >= 0 && upper_index >= 0) {
				// found both lower and upper index, create a BETWEEN expression
				auto lower_constant = make_unique<BoundConstantExpression>(constant_list[lower_index].constant);
				auto upper_constant = make_unique<BoundConstantExpression>(constant_list[upper_index].constant);
				auto between = make_unique<BoundBetweenExpression>(
				    entries[i]->Copy(), move(lower_constant), move(upper_constant), lower_inclusive, upper_inclusive);
				callback(move(between));
			} else if (lower_index >= 0) {
				// only lower index found, create simple comparison expression
				auto constant = make_unique<BoundConstantExpression>(constant_list[lower_index].constant);
				auto comparison = make_unique<BoundComparisonExpression>(constant_list[lower_index].comparison_type,
				                                                         entries[i]->Copy(), move(constant));
				callback(move(comparison));
			} else if (upper_index >= 0) {
				// only upper index found, create simple comparison expression
				auto constant = make_unique<BoundConstantExpression>(constant_list[upper_index].constant);
				auto comparison = make_unique<BoundComparisonExpression>(constant_list[upper_index].comparison_type,
				                                                         entries[i]->Copy(), move(constant));
				callback(move(comparison));
			}
		}
	}
	stored_expressions.clear();
	equivalence_set_map.clear();
	constant_values.clear();
	equivalence_map.clear();
}

bool FilterCombiner::HasFilters() {
	bool has_filters = false;
	GenerateFilters([&](unique_ptr<Expression> child) { has_filters = true; });
	return has_filters;
}

void FilterCombiner::FindZonemapChecks(vector<idx_t> &column_ids, unordered_map<idx_t, std::pair<Value, Value>> &checks,
                                       unordered_set<idx_t> &not_constants, Expression *filter) {
	switch (filter->type) {
	case ExpressionType::CONJUNCTION_OR: {
		auto &or_exp = (BoundConjunctionExpression &)*filter;
		for (auto &child : or_exp.children) {
			FindZonemapChecks(column_ids, checks, not_constants, child.get());
		}
		break;
	}
	case ExpressionType::CONJUNCTION_AND: {
		auto &and_exp = (BoundConjunctionExpression &)*filter;
		for (auto &child : and_exp.children) {
			FindZonemapChecks(column_ids, checks, not_constants, child.get());
		}
		break;
	}
	case ExpressionType::COMPARE_IN: {
		auto &comp_in_exp = (BoundOperatorExpression &)*filter;
		if (comp_in_exp.children[0]->type == ExpressionType::BOUND_COLUMN_REF) {
			auto &column_ref = (BoundColumnRefExpression &)*comp_in_exp.children[0].get();
			for (size_t i{1}; i < comp_in_exp.children.size(); i++) {
				if (comp_in_exp.children[i]->type != ExpressionType::VALUE_CONSTANT) {
					//! This indicates the column has a comparison that is not with a constant
					not_constants.insert(column_ids[column_ref.binding.column_index]);
					break;
				} else {
					auto &const_value_expr = (BoundConstantExpression &)*comp_in_exp.children[i].get();
					auto it = checks.find(column_ids[column_ref.binding.column_index]);
					if (it == checks.end()) {
						checks[column_ids[column_ref.binding.column_index]] = {const_value_expr.value,
						                                                       const_value_expr.value};
					} else {
						if (it->second.first > const_value_expr.value) {
							it->second.first = const_value_expr.value;
						}
						if (it->second.second < const_value_expr.value) {
							it->second.second = const_value_expr.value;
						}
					}
				}
			}
		}
		break;
	}
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHAN: {
		auto &comp_exp = (BoundComparisonExpression &)*filter;
		if ((comp_exp.left->expression_class == ExpressionClass::BOUND_COLUMN_REF &&
		     comp_exp.right->expression_class == ExpressionClass::BOUND_CONSTANT)) {
			auto &column_ref = (BoundColumnRefExpression &)*comp_exp.left;
			auto &constant_value_expr = (BoundConstantExpression &)*comp_exp.right;
			auto it = checks.find(column_ids[column_ref.binding.column_index]);
			if (it == checks.end()) {
				checks[column_ids[column_ref.binding.column_index]] = {constant_value_expr.value,
				                                                       constant_value_expr.value};
			} else {
				if (it->second.first > constant_value_expr.value) {
					it->second.first = constant_value_expr.value;
				}
				if (it->second.second < constant_value_expr.value) {
					it->second.second = constant_value_expr.value;
				}
			}
		} else if (comp_exp.left->expression_class == ExpressionClass::BOUND_COLUMN_REF) {
			//! This indicates the column has a comparison that is not with a constant
			auto &column_ref = (BoundColumnRefExpression &)*comp_exp.left;
			not_constants.insert(column_ids[column_ref.binding.column_index]);
		}
		break;
	}
	default:
		break;
	}
}

vector<TableFilter> FilterCombiner::GenerateZonemapChecks(vector<idx_t> &column_ids,
                                                          vector<TableFilter> &pushed_filters) {
	vector<TableFilter> zonemap_checks;
	unordered_map<idx_t, std::pair<Value, Value>> checks;
	unordered_set<idx_t> not_constants;
	//! We go through the remaining filters and capture their min max
	for (auto &filter : remaining_filters) {
		FindZonemapChecks(column_ids, checks, not_constants, filter.get());
	}
	//! We construct the equivalent filters
	for (auto not_constant : not_constants) {
		checks.erase(not_constant);
	}
	for (const auto &pushed_filter : pushed_filters) {
		checks.erase(column_ids[pushed_filter.column_index]);
	}
	for (const auto &check : checks) {
		zonemap_checks.emplace_back(check.second.first, ExpressionType::COMPARE_GREATERTHANOREQUALTO, check.first);
		zonemap_checks.emplace_back(check.second.second, ExpressionType::COMPARE_LESSTHANOREQUALTO, check.first);
	}
	return zonemap_checks;
}

vector<TableFilter> FilterCombiner::GenerateTableScanFilters(vector<idx_t> &column_ids) {
	vector<TableFilter> tableFilters;
	//! First, we figure the filters that have constant expressions that we can push down to the table scan
	for (auto &constant_value : constant_values) {
		if (!constant_value.second.empty()) {
			auto filter_exp = equivalence_map.end();
			if ((constant_value.second[0].comparison_type == ExpressionType::COMPARE_EQUAL ||
			     constant_value.second[0].comparison_type == ExpressionType::COMPARE_GREATERTHAN ||
			     constant_value.second[0].comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
			     constant_value.second[0].comparison_type == ExpressionType::COMPARE_LESSTHAN ||
			     constant_value.second[0].comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO) &&
			    (TypeIsNumeric(constant_value.second[0].constant.type().InternalType()) ||
			     constant_value.second[0].constant.type().InternalType() == PhysicalType::VARCHAR)) {
				//! Here we check if these filters are column references
				filter_exp = equivalence_map.find(constant_value.first);
				if (filter_exp->second.size() == 1 && filter_exp->second[0]->type == ExpressionType::BOUND_COLUMN_REF) {
					auto filter_col_exp = static_cast<BoundColumnRefExpression *>(filter_exp->second[0]);
					if (column_ids[filter_col_exp->binding.column_index] == COLUMN_IDENTIFIER_ROW_ID) {
						break;
					}
					auto equivalence_set = filter_exp->first;
					auto &entries = filter_exp->second;
					auto &constant_list = constant_values.find(equivalence_set)->second;
					// for each entry generate an equality expression comparing to each other
					for (idx_t i = 0; i < entries.size(); i++) {
						// for each entry also create a comparison with each constant
						for (idx_t k = 0; k < constant_list.size(); k++) {
							tableFilters.emplace_back(constant_value.second[k].constant,
							                          constant_value.second[k].comparison_type,
							                          filter_col_exp->binding.column_index);
						}
					}
					equivalence_map.erase(filter_exp);
				}
			}
		}
	}
	//! Here we look for LIKE or IN filters
	for (size_t rem_fil_idx{}; rem_fil_idx < remaining_filters.size(); rem_fil_idx++) {
		auto &remaining_filter = remaining_filters[rem_fil_idx];
		if (remaining_filter->expression_class == ExpressionClass::BOUND_FUNCTION) {
			auto &func = (BoundFunctionExpression &)*remaining_filter;
			if (func.function.name == "prefix" &&
			    func.children[0]->expression_class == ExpressionClass::BOUND_COLUMN_REF &&
			    func.children[1]->type == ExpressionType::VALUE_CONSTANT) {
				//! This is a like function.
				auto &column_ref = (BoundColumnRefExpression &)*func.children[0].get();
				auto &constant_value_expr = (BoundConstantExpression &)*func.children[1].get();
				string like_string = constant_value_expr.value.str_value;
				if (like_string.empty()) {
					continue;
				}
				auto const_value = constant_value_expr.value.Copy();
				const_value.str_value = like_string;
				//! Here the like must be transformed to a BOUND COMPARISON geq le
				tableFilters.emplace_back(const_value, ExpressionType::COMPARE_GREATERTHANOREQUALTO,
				                          column_ref.binding.column_index);
				const_value.str_value[const_value.str_value.size() - 1]++;
				tableFilters.emplace_back(const_value, ExpressionType::COMPARE_LESSTHAN,
				                          column_ref.binding.column_index);
			}
			if (func.function.name == "~~" && func.children[0]->expression_class == ExpressionClass::BOUND_COLUMN_REF &&
			    func.children[1]->type == ExpressionType::VALUE_CONSTANT) {
				//! This is a like function.
				auto &column_ref = (BoundColumnRefExpression &)*func.children[0].get();
				auto &constant_value_expr = (BoundConstantExpression &)*func.children[1].get();
				string like_string = constant_value_expr.value.str_value;
				auto const_value = constant_value_expr.value.Copy();
				if (like_string[0] == '%' || like_string[0] == '_') {
					//! We have no prefix so nothing to pushdown
					break;
				}
				string prefix;
				bool equality = true;
				for (char const &c : like_string) {
					if (c == '%' || c == '_') {
						equality = false;
						break;
					}
					prefix += c;
				}
				const_value.str_value = prefix;
				if (equality) {
					//! Here the like can be transformed to an equality query
					tableFilters.emplace_back(const_value, ExpressionType::COMPARE_EQUAL,
					                          column_ref.binding.column_index);
				} else {
					//! Here the like must be transformed to a BOUND COMPARISON geq le
					tableFilters.emplace_back(const_value, ExpressionType::COMPARE_GREATERTHANOREQUALTO,
					                          column_ref.binding.column_index);
					const_value.str_value[const_value.str_value.size() - 1]++;
					tableFilters.emplace_back(const_value, ExpressionType::COMPARE_LESSTHAN,
					                          column_ref.binding.column_index);
				}
			}
		}
		else if (remaining_filter->type == ExpressionType::COMPARE_IN) {
			auto &func = (BoundOperatorExpression &)*remaining_filter;
			D_ASSERT(func.children.size() > 1);
			if (func.children[0]->expression_class == ExpressionClass::BOUND_COLUMN_REF) {
				auto &column_ref = (BoundColumnRefExpression &)*func.children[0].get();
				if (column_ids[column_ref.binding.column_index] == COLUMN_IDENTIFIER_ROW_ID) {
						break;
				}
				//! check if all children are const expr
				bool children_constant = true;
				for (size_t i{1}; i < func.children.size(); i++) {
					if (func.children[i]->type != ExpressionType::VALUE_CONSTANT) {
						children_constant = false;
					}
				}
				if (children_constant) {
					auto &fst_const_value_expr = (BoundConstantExpression &)*func.children[1].get();
					//! Check if values are consecutive, if yes transform them to >= <= (only for integers)
					if (fst_const_value_expr.value.type() == LogicalType::BIGINT ||
					    fst_const_value_expr.value.type() == LogicalType::INTEGER ||
					    fst_const_value_expr.value.type() == LogicalType::SMALLINT ||
					    fst_const_value_expr.value.type() == LogicalType::TINYINT ||
					    fst_const_value_expr.value.type() == LogicalType::HUGEINT) {
						vector<Value> in_values;
						for (size_t i{1}; i < func.children.size(); i++) {
							auto &const_value_expr = (BoundConstantExpression &)*func.children[i].get();
							in_values.push_back(const_value_expr.value);
						}
						Value one(1);
						bool is_consecutive = true;
						sort(in_values.begin(), in_values.end());
						for (size_t in_val_idx{1}; in_val_idx < in_values.size(); in_val_idx++) {
							if (in_values[in_val_idx] - in_values[in_val_idx - 1] > one ||
							    in_values[in_val_idx - 1].is_null) {
								is_consecutive = false;
							}
						}
						if (is_consecutive && !in_values.empty()) {
							tableFilters.emplace_back(in_values.front(), ExpressionType::COMPARE_GREATERTHANOREQUALTO,
							                          column_ref.binding.column_index);
							tableFilters.emplace_back(in_values.back(), ExpressionType::COMPARE_LESSTHANOREQUALTO,
							                          column_ref.binding.column_index);
						} else {
							//! Is not consecutive, we execute filter normally
							continue;
						}
					} else {
						//! Is not integer, we execute filter normally
						continue;
					}
				} else {
					//! Not all values are constant, we execute filter normally
					continue;
				}
				remaining_filters.erase(remaining_filters.begin() + rem_fil_idx);
			}
		}
	}

	return tableFilters;
}

static bool IsGreaterThan(ExpressionType type) {
	return type == ExpressionType::COMPARE_GREATERTHAN || type == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
}

static bool IsLessThan(ExpressionType type) {
	return type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_LESSTHANOREQUALTO;
}

FilterResult FilterCombiner::AddBoundComparisonFilter(Expression *expr) {
	auto &comparison = (BoundComparisonExpression &)*expr;
	if (comparison.type != ExpressionType::COMPARE_LESSTHAN &&
	    comparison.type != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
	    comparison.type != ExpressionType::COMPARE_GREATERTHAN &&
	    comparison.type != ExpressionType::COMPARE_GREATERTHANOREQUALTO &&
	    comparison.type != ExpressionType::COMPARE_EQUAL && comparison.type != ExpressionType::COMPARE_NOTEQUAL) {
		// only support [>, >=, <, <=, ==] expressions
		return FilterResult::UNSUPPORTED;
	}
	// check if one of the sides is a scalar value
	bool left_is_scalar = comparison.left->IsFoldable();
	bool right_is_scalar = comparison.right->IsFoldable();
	if (left_is_scalar || right_is_scalar) {
		// comparison with scalar
		auto node = GetNode(left_is_scalar ? comparison.right.get() : comparison.left.get());
		idx_t equivalence_set = GetEquivalenceSet(node);
		auto scalar = left_is_scalar ? comparison.left.get() : comparison.right.get();
		auto constant_value = ExpressionExecutor::EvaluateScalar(*scalar);

		// create the ExpressionValueInformation
		ExpressionValueInformation info;
		info.comparison_type = left_is_scalar ? FlipComparisionExpression(comparison.type) : comparison.type;
		info.constant = constant_value;

		// get the current bucket of constant values
		D_ASSERT(constant_values.find(equivalence_set) != constant_values.end());
		auto &info_list = constant_values.find(equivalence_set)->second;
		// check the existing constant comparisons to see if we can do any pruning
		auto ret = AddConstantComparison(info_list, info);

		auto non_scalar = left_is_scalar ? comparison.right.get() : comparison.left.get();
		auto transitive_filter = FindTransitiveFilter(non_scalar);
		if (transitive_filter != nullptr) {
			// try to add transitive filters
			if (AddTransitiveFilters((BoundComparisonExpression &)*transitive_filter) == FilterResult::UNSUPPORTED) {
				// in case of unsuccessful re-add filter into remaining ones
				remaining_filters.push_back(move(transitive_filter));
			}
		}
		return ret;

	} else {
		// comparison between two non-scalars
		// only handle comparisons for now
		if (expr->type != ExpressionType::COMPARE_EQUAL) {
			if (IsGreaterThan(expr->type) || IsLessThan(expr->type)) {
				return AddTransitiveFilters(comparison);
			}
			return FilterResult::UNSUPPORTED;
		}
		// get the LHS and RHS nodes
		auto left_node = GetNode(comparison.left.get());
		auto right_node = GetNode(comparison.right.get());
		if (BaseExpression::Equals(left_node, right_node)) {
			return FilterResult::UNSUPPORTED;
		}
		// get the equivalence sets of the LHS and RHS
		auto left_equivalence_set = GetEquivalenceSet(left_node);
		auto right_equivalence_set = GetEquivalenceSet(right_node);
		if (left_equivalence_set == right_equivalence_set) {
			// this equality filter already exists, prune it
			return FilterResult::SUCCESS;
		}
		// add the right bucket into the left bucket
		D_ASSERT(equivalence_map.find(left_equivalence_set) != equivalence_map.end());
		D_ASSERT(equivalence_map.find(right_equivalence_set) != equivalence_map.end());

		auto &left_bucket = equivalence_map.find(left_equivalence_set)->second;
		auto &right_bucket = equivalence_map.find(right_equivalence_set)->second;
		for (auto &i : right_bucket) {
			// rewrite the equivalence set mapping for this node
			equivalence_set_map[i] = left_equivalence_set;
			// add the node to the left bucket
			left_bucket.push_back(i);
		}
		// now add all constant values from the right bucket to the left bucket
		D_ASSERT(constant_values.find(left_equivalence_set) != constant_values.end());
		D_ASSERT(constant_values.find(right_equivalence_set) != constant_values.end());
		auto &left_constant_bucket = constant_values.find(left_equivalence_set)->second;
		auto &right_constant_bucket = constant_values.find(right_equivalence_set)->second;
		for (auto &i : right_constant_bucket) {
			if (AddConstantComparison(left_constant_bucket, i) == FilterResult::UNSATISFIABLE) {
				return FilterResult::UNSATISFIABLE;
			}
		}
	}
	return FilterResult::SUCCESS;
}

FilterResult FilterCombiner::AddFilter(Expression *expr) {
	if (expr->HasParameter()) {
		return FilterResult::UNSUPPORTED;
	}
	if (expr->IsFoldable()) {
		// scalar condition, evaluate it
		auto result = ExpressionExecutor::EvaluateScalar(*expr).CastAs(LogicalType::BOOLEAN);
		// check if the filter passes
		if (result.is_null || !result.value_.boolean) {
			// the filter does not pass the scalar test, create an empty result
			return FilterResult::UNSATISFIABLE;
		} else {
			// the filter passes the scalar test, just remove the condition
			return FilterResult::SUCCESS;
		}
	}
	D_ASSERT(!expr->IsFoldable());
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_BETWEEN) {
		auto &comparison = (BoundBetweenExpression &)*expr;
		//! check if one of the sides is a scalar value
		bool left_is_scalar = comparison.lower->IsFoldable();
		bool right_is_scalar = comparison.upper->IsFoldable();
		if (left_is_scalar || right_is_scalar) {
			//! comparison with scalar
			auto node = GetNode(comparison.input.get());
			idx_t equivalence_set = GetEquivalenceSet(node);
			auto scalar = comparison.lower.get();
			auto constant_value = ExpressionExecutor::EvaluateScalar(*scalar);

			// create the ExpressionValueInformation
			ExpressionValueInformation info;
			if (comparison.lower_inclusive) {
				info.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
			} else {
				info.comparison_type = ExpressionType::COMPARE_GREATERTHAN;
			}
			info.constant = constant_value;

			// get the current bucket of constant values
			D_ASSERT(constant_values.find(equivalence_set) != constant_values.end());
			auto &info_list = constant_values.find(equivalence_set)->second;
			// check the existing constant comparisons to see if we can do any pruning
			AddConstantComparison(info_list, info);
			scalar = comparison.upper.get();
			constant_value = ExpressionExecutor::EvaluateScalar(*scalar);

			// create the ExpressionValueInformation
			if (comparison.upper_inclusive) {
				info.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
			} else {
				info.comparison_type = ExpressionType::COMPARE_LESSTHAN;
			}
			info.constant = constant_value;

			// get the current bucket of constant values
			D_ASSERT(constant_values.find(equivalence_set) != constant_values.end());
			// check the existing constant comparisons to see if we can do any pruning
			return AddConstantComparison(constant_values.find(equivalence_set)->second, info);
		}
	} else if (expr->GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		return AddBoundComparisonFilter(expr);
	}
	// only comparisons supported for now
	return FilterResult::UNSUPPORTED;
}

/*
 * Create and add new transitive filters from a two non-scalar filter such as j > i, j >= i, j < i, and j <= i
 * It's missing to create another method to add transitive filters from scalar filters, e.g, i > 10
 */
FilterResult FilterCombiner::AddTransitiveFilters(BoundComparisonExpression &comparison) {
	D_ASSERT(IsGreaterThan(comparison.type) || IsLessThan(comparison.type));
	// get the LHS and RHS nodes
	Expression *left_node = GetNode(comparison.left.get());
	Expression *right_node = GetNode(comparison.right.get());
	if (BaseExpression::Equals(left_node, right_node)) {
		return FilterResult::UNSUPPORTED;
	}
	// get the equivalence sets of the LHS and RHS
	idx_t left_equivalence_set = GetEquivalenceSet(left_node);
	idx_t right_equivalence_set = GetEquivalenceSet(right_node);
	if (left_equivalence_set == right_equivalence_set) {
		// this equality filter already exists, prune it
		return FilterResult::SUCCESS;
	}

	vector<ExpressionValueInformation> &left_constants = constant_values.find(left_equivalence_set)->second;
	vector<ExpressionValueInformation> &right_constants = constant_values.find(right_equivalence_set)->second;
	bool isSuccessful = false;
	bool isInserted = false;
	// read every constant filters already inserted for the right scalar variable
	// and see if we can create new transitive filters, e.g., there is already a filter i > 10,
	// suppose that we have now the j >= i, then we can infer a new filter j > 10
	for (const auto &right_constant : right_constants) {
		ExpressionValueInformation info;
		info.constant = right_constant.constant;
		// there is already an equality filter, e.g., i = 10
		if (right_constant.comparison_type == ExpressionType::COMPARE_EQUAL) {
			// create filter j [>, >=, <, <=] 10
			// suppose the new comparison is j >= i and we have already a filter i = 10,
			// then we create a new filter j >= 10
			// and the filter j >= i can be pruned by not adding it into the remaining filters
			info.comparison_type = comparison.type;
		} else if ((comparison.type == ExpressionType::COMPARE_GREATERTHANOREQUALTO &&
		            IsGreaterThan(right_constant.comparison_type)) ||
		           (comparison.type == ExpressionType::COMPARE_LESSTHANOREQUALTO &&
		            IsLessThan(right_constant.comparison_type))) {
			// filters (j >= i AND i [>, >=] 10) OR (j <= i AND i [<, <=] 10)
			// create filter j [>, >=] 10 and add the filter j [>=, <=] i into the remaining filters
			info.comparison_type = right_constant.comparison_type; // create filter j [>, >=, <, <=] 10
			if (!isInserted) {
				// Add the filter j >= i in the remaing filters
				auto filter = make_unique<BoundComparisonExpression>(comparison.type, comparison.left->Copy(),
				                                                     comparison.right->Copy());
				remaining_filters.push_back(move(filter));
				isInserted = true;
			}
		} else if ((comparison.type == ExpressionType::COMPARE_GREATERTHAN &&
		            IsGreaterThan(right_constant.comparison_type)) ||
		           (comparison.type == ExpressionType::COMPARE_LESSTHAN &&
		            IsLessThan(right_constant.comparison_type))) {
			// filters (j > i AND i [>, >=] 10) OR j < i AND i [<, <=] 10
			// create filter j [>, <] 10 and add the filter j [>, <] i into the remaining filters
			// the comparisons j > i and j < i are more restrictive
			info.comparison_type = comparison.type;
			if (!isInserted) {
				// Add the filter j [>, <] i
				auto filter = make_unique<BoundComparisonExpression>(comparison.type, comparison.left->Copy(),
				                                                     comparison.right->Copy());
				remaining_filters.push_back(move(filter));
				isInserted = true;
			}
		} else {
			// we cannot add a new filter
			continue;
		}
		// Add the new filer into the left set
		if (AddConstantComparison(left_constants, info) == FilterResult::UNSATISFIABLE) {
			return FilterResult::UNSATISFIABLE;
		}
		isSuccessful = true;
	}
	if (isSuccessful) {
		// now check for remaining trasitive filters from the left column
		auto transitive_filter = FindTransitiveFilter(comparison.left.get());
		if (transitive_filter != nullptr) {
			// try to add transitive filters
			if (AddTransitiveFilters((BoundComparisonExpression &)*transitive_filter) == FilterResult::UNSUPPORTED) {
				// in case of unsuccessful re-add filter into remaining ones
				remaining_filters.push_back(move(transitive_filter));
			}
		}
		return FilterResult::SUCCESS;
	}

	return FilterResult::UNSUPPORTED;
}

/*
 * Find a transitive filter already inserted into the remaining filters
 * Check for a match between the right column of bound comparisons and the expression,
 * then removes the bound comparison from the remaining filters and returns it
 */
unique_ptr<Expression> FilterCombiner::FindTransitiveFilter(Expression *expr) {
	// We only check for bound column ref
	if (expr->type == ExpressionType::BOUND_COLUMN_REF) {
		for (idx_t i = 0; i < remaining_filters.size(); i++) {
			if (remaining_filters[i]->GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
				auto comparison = (BoundComparisonExpression *)remaining_filters[i].get();
				if (expr->Equals(comparison->right.get()) && comparison->type != ExpressionType::COMPARE_NOTEQUAL) {
					auto filter = move(remaining_filters[i]);
					remaining_filters.erase(remaining_filters.begin() + i);
					return filter;
				}
			}
		}
	}
	return nullptr;
}

ValueComparisonResult InvertValueComparisonResult(ValueComparisonResult result) {
	if (result == ValueComparisonResult::PRUNE_RIGHT) {
		return ValueComparisonResult::PRUNE_LEFT;
	}
	if (result == ValueComparisonResult::PRUNE_LEFT) {
		return ValueComparisonResult::PRUNE_RIGHT;
	}
	return result;
}

ValueComparisonResult CompareValueInformation(ExpressionValueInformation &left, ExpressionValueInformation &right) {
	if (left.comparison_type == ExpressionType::COMPARE_EQUAL) {
		// left is COMPARE_EQUAL, we can either
		// (1) prune the right side or
		// (2) return UNSATISFIABLE
		bool prune_right_side = false;
		switch (right.comparison_type) {
		case ExpressionType::COMPARE_LESSTHAN:
			prune_right_side = left.constant < right.constant;
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			prune_right_side = left.constant <= right.constant;
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			prune_right_side = left.constant > right.constant;
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			prune_right_side = left.constant >= right.constant;
			break;
		case ExpressionType::COMPARE_NOTEQUAL:
			prune_right_side = left.constant != right.constant;
			break;
		default:
			D_ASSERT(right.comparison_type == ExpressionType::COMPARE_EQUAL);
			prune_right_side = left.constant == right.constant;
			break;
		}
		if (prune_right_side) {
			return ValueComparisonResult::PRUNE_RIGHT;
		} else {
			return ValueComparisonResult::UNSATISFIABLE_CONDITION;
		}
	} else if (right.comparison_type == ExpressionType::COMPARE_EQUAL) {
		// right is COMPARE_EQUAL
		return InvertValueComparisonResult(CompareValueInformation(right, left));
	} else if (left.comparison_type == ExpressionType::COMPARE_NOTEQUAL) {
		// left is COMPARE_NOTEQUAL, we can either
		// (1) prune the left side or
		// (2) not prune anything
		bool prune_left_side = false;
		switch (right.comparison_type) {
		case ExpressionType::COMPARE_LESSTHAN:
			prune_left_side = left.constant >= right.constant;
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			prune_left_side = left.constant > right.constant;
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			prune_left_side = left.constant <= right.constant;
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			prune_left_side = left.constant < right.constant;
			break;
		default:
			D_ASSERT(right.comparison_type == ExpressionType::COMPARE_NOTEQUAL);
			prune_left_side = left.constant == right.constant;
			break;
		}
		if (prune_left_side) {
			return ValueComparisonResult::PRUNE_LEFT;
		} else {
			return ValueComparisonResult::PRUNE_NOTHING;
		}
	} else if (right.comparison_type == ExpressionType::COMPARE_NOTEQUAL) {
		return InvertValueComparisonResult(CompareValueInformation(right, left));
	} else if (IsGreaterThan(left.comparison_type) && IsGreaterThan(right.comparison_type)) {
		// both comparisons are [>], we can either
		// (1) prune the left side or
		// (2) prune the right side
		if (left.constant > right.constant) {
			// left constant is more selective, prune right
			return ValueComparisonResult::PRUNE_RIGHT;
		} else if (left.constant < right.constant) {
			// right constant is more selective, prune left
			return ValueComparisonResult::PRUNE_LEFT;
		} else {
			// constants are equivalent
			// however we can still have the scenario where one is [>=] and the other is [>]
			// we want to prune the [>=] because [>] is more selective
			// if left is [>=] we prune the left, else we prune the right
			if (left.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
				return ValueComparisonResult::PRUNE_LEFT;
			} else {
				return ValueComparisonResult::PRUNE_RIGHT;
			}
		}
	} else if (IsLessThan(left.comparison_type) && IsLessThan(right.comparison_type)) {
		// both comparisons are [<], we can either
		// (1) prune the left side or
		// (2) prune the right side
		if (left.constant < right.constant) {
			// left constant is more selective, prune right
			return ValueComparisonResult::PRUNE_RIGHT;
		} else if (left.constant > right.constant) {
			// right constant is more selective, prune left
			return ValueComparisonResult::PRUNE_LEFT;
		} else {
			// constants are equivalent
			// however we can still have the scenario where one is [<=] and the other is [<]
			// we want to prune the [<=] because [<] is more selective
			// if left is [<=] we prune the left, else we prune the right
			if (left.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
				return ValueComparisonResult::PRUNE_LEFT;
			} else {
				return ValueComparisonResult::PRUNE_RIGHT;
			}
		}
	} else if (IsLessThan(left.comparison_type)) {
		D_ASSERT(IsGreaterThan(right.comparison_type));
		// left is [<] and right is [>], in this case we can either
		// (1) prune nothing or
		// (2) return UNSATISFIABLE
		// the SMALLER THAN constant has to be greater than the BIGGER THAN constant
		if (left.constant >= right.constant) {
			return ValueComparisonResult::PRUNE_NOTHING;
		} else {
			return ValueComparisonResult::UNSATISFIABLE_CONDITION;
		}
	} else {
		// left is [>] and right is [<] or [!=]
		D_ASSERT(IsLessThan(right.comparison_type) && IsGreaterThan(left.comparison_type));
		return InvertValueComparisonResult(CompareValueInformation(right, left));
	}
}

} // namespace duckdb
