//===----------------------------------------------------------------------===//
//                         DuckDB
//
// execution/index/art/art.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/common.hpp"
#include "common/types/data_chunk.hpp"
#include "common/types/tuple.hpp"
#include "common/types/vector.hpp"
#include "parser/parsed_expression.hpp"
#include "storage/data_table.hpp"
#include "storage/index.hpp"
#include "common/types/static_vector.hpp"
#include "art_key.hpp"
#include "leaf.hpp"
#include "node.hpp"
#include "node4.hpp"
#include "node16.hpp"
#include "node48.hpp"
#include "node256.hpp"

namespace duckdb {
struct IteratorEntry {
	Node *node = nullptr;
	index_t pos = 0;
};

struct Iterator {
	//! The current Leaf Node, valid if depth>0
	Leaf *node = nullptr;
	//! The current depth
	int32_t depth = 0;
	//! Stack, actually the size is determined at runtime
	IteratorEntry stack[9];

	bool start = false;
};

struct ARTIndexScanState : public IndexScanState {
	ARTIndexScanState(vector<column_t> column_ids) : IndexScanState(column_ids), checked(false), result_index(0) {
	}

	Value values[2];
	ExpressionType expressions[2];
	bool checked;
	index_t result_index = 0;
	vector<row_t> result_ids;
	Iterator iterator;
};

class ART : public Index {
public:
	ART(DataTable &table, vector<column_t> column_ids, vector<unique_ptr<Expression>> unbound_expressions,
	    bool is_unique = false);
	~ART();

	//! Lock used for updating the index
	std::mutex lock;
	//! Root of the tree
	unique_ptr<Node> tree;
	//! True if machine is little endian
	bool is_little_endian;
	//! The maximum prefix length for compressed paths stored in the
	//! header, if the path is longer it is loaded from the database on demand
	uint32_t maxPrefix;
	//! Whether or not the ART is an index built to enforce a UNIQUE constraint
	bool is_unique;

public:
	//! Initialize a scan on the index with the given expression and column ids
	//! to fetch from the base table for a single predicate
	unique_ptr<IndexScanState> InitializeScanSinglePredicate(Transaction &transaction, vector<column_t> column_ids,
	                                                         Value value, ExpressionType expressionType) override;

	//! Initialize a scan on the index with the given expression and column ids
	//! to fetch from the base table for two predicates
	unique_ptr<IndexScanState> InitializeScanTwoPredicates(Transaction &transaction, vector<column_t> column_ids,
	                                                       Value low_value, ExpressionType low_expression_type,
	                                                       Value high_value,
	                                                       ExpressionType high_expression_type) override;

	//! Perform a lookup on the index
	void Scan(Transaction &transaction, IndexScanState *ss, DataChunk &result) override;
	//! Append entries to the index
	bool Append(DataChunk &entries, Vector &row_identifiers) override;
	//! Delete entries in the index
	void Delete(DataChunk &entries, Vector &row_identifiers) override;

	//! Insert data into the index. Does not lock the index.
	bool Insert(DataChunk &data, Vector &row_ids);

private:
	DataChunk expression_result;

private:
	//! Insert a row id into a leaf node
	bool InsertToLeaf(Leaf &leaf, row_t row_id);
	//! Insert the leaf value into the tree
	bool Insert(unique_ptr<Node> &node, unique_ptr<Key> key, unsigned depth, row_t row_id);

	//! Erase element from leaf (if leaf has more than one value) or eliminate the leaf itself
	void Erase(unique_ptr<Node> &node, Key &key, unsigned depth, row_t row_id);

	//! Check if the key of the leaf is equal to the searched key
	bool LeafMatches(Node *node, Key &key, unsigned depth);

	//! Find the node with a matching key, optimistic version
	Node *Lookup(unique_ptr<Node> &node, Key &key, unsigned depth);

	//! Find the first node that is bigger (or equal to) a specific key
	bool Bound(unique_ptr<Node> &node, Key &key, Iterator &iterator, bool inclusive);

	//! Gets next node for range queries
	bool IteratorNext(Iterator &iter);

	void SearchEqual(vector<row_t> &result_ids, ARTIndexScanState *state);
	void SearchGreater(vector<row_t> &result_ids, ARTIndexScanState *state, bool inclusive);
	void SearchLess(vector<row_t> &result_ids, ARTIndexScanState *state, bool inclusive);
	void SearchCloseRange(vector<row_t> &result_ids, ARTIndexScanState *state, bool left_inclusive,
	                      bool right_inclusive);

private:
	template <bool HAS_BOUND, bool INCLUSIVE>
	void IteratorScan(ARTIndexScanState *state, Iterator *it, vector<row_t> &result_ids, Key *upper_bound);

	void GenerateKeys(DataChunk &input, vector<unique_ptr<Key>> &keys);
};

} // namespace duckdb