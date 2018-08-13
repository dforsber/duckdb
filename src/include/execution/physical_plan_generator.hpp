//===----------------------------------------------------------------------===//
//
//                         DuckDB
//
// execution/physical_plan_generator.hpp
//
// Author: Mark Raasveldt
//
//===----------------------------------------------------------------------===//

#pragma once

#include <vector>

#include "common/internal_types.hpp"
#include "common/printable.hpp"

#include "execution/physical_operator.hpp"

#include "planner/bindcontext.hpp"
#include "planner/logical_operator.hpp"
#include "planner/logical_operator_visitor.hpp"

namespace duckdb {

struct TableColumnInformation {
	size_t column_offset;
	size_t column_count;
};

//! The physical plan generator generates a physical execution plan from a
//! logical query plan
class PhysicalPlanGenerator : public LogicalOperatorVisitor {
  public:
	PhysicalPlanGenerator(Catalog &catalog,
	                      PhysicalPlanGenerator *parent = nullptr)
	    : catalog(catalog), parent(parent) {}

	bool CreatePlan(std::unique_ptr<LogicalOperator> logical,
	                std::unique_ptr<BindContext> context);

	bool GetSuccess() const { return success; }
	const std::string &GetErrorMessage() const { return message; }

	void Visit(LogicalAggregate &op);
	void Visit(LogicalCrossProduct &op);
	void Visit(LogicalDistinct &op);
	void Visit(LogicalFilter &op);
	void Visit(LogicalGet &op);
	void Visit(LogicalJoin &op);
	void Visit(LogicalLimit &op);
	void Visit(LogicalOrder &op);
	void Visit(LogicalProjection &op);
	void Visit(LogicalInsert &op);
	void Visit(LogicalCopy &op);

	void Visit(ColumnRefExpression &expr);
	void Visit(SubqueryExpression &expr);

	void Print() { plan->Print(); }

	std::unique_ptr<PhysicalOperator> plan;
	std::unique_ptr<BindContext> context;
	std::unordered_map<size_t, TableColumnInformation> table_index_map;

	PhysicalPlanGenerator *parent;

	bool success;
	std::string message;

  private:
	Catalog &catalog;
};
} // namespace duckdb