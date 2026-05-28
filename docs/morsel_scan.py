# # Morsel Scan Pseudocode
#
# This file is intentionally Python-like pseudocode. It documents how the
# scan-node-table operator, morsel assignment, ice-disk relationship scans, and
# Parquet reader cooperate. It is not executable production code.

DEFAULT_VECTOR_CAPACITY = 2048


# ## Parquet Reader
#
# The Parquet reader is vector-oriented. A call to `scan()` returns at most one
# execution vector, normally 2048 rows. It does not mean "read the full row
# group". Callers must keep invoking it until it returns an empty chunk.


class ParquetReaderScanState:
    def __init__(self):
        self.group_idx_list = []      # Parquet row groups assigned to this scan.
        self.current_group_pos = -1   # Position inside group_idx_list.
        self.group_offset = 0         # Row offset inside the current row group.
        self.finished = False


class ParquetReader:
    def initialize_scan(self, state, row_groups_to_read):
        state.group_idx_list = row_groups_to_read
        state.current_group_pos = -1
        state.group_offset = 0
        state.finished = False

        # Real implementation also opens the file, creates thrift protocol
        # state, builds column readers, and allocates definition/repetition
        # buffers.

    def scan_internal(self, state, output_chunk):
        if state.finished:
            output_chunk.size = 0
            return False

        if self._need_next_row_group(state):
            state.current_group_pos += 1
            state.group_offset = 0

            if state.current_group_pos == len(state.group_idx_list):
                state.finished = True
                output_chunk.size = 0
                return False

            self._prepare_current_row_group(state)
            output_chunk.size = 0
            return True

        row_group = self._current_row_group(state)
        rows_to_read = min(
            DEFAULT_VECTOR_CAPACITY,
            row_group.num_rows - state.group_offset,
        )

        if rows_to_read == 0:
            state.finished = True
            output_chunk.size = 0
            return False

        output_chunk.size = rows_to_read

        for column_reader, output_vector in zip(self.column_readers, output_chunk.vectors):
            column_reader.read(
                num_rows=rows_to_read,
                row_group_offset=state.group_offset,
                into=output_vector,
            )

        state.group_offset += rows_to_read
        return True

    def scan(self, state, output_chunk):
        # Return the first non-empty vector. This helper is convenient, but it
        # is still only a single vector. Treating it as "all rows" caused the
        # historical ice-disk 2048-row cap.
        output_chunk.size = 0
        while self.scan_internal(state, output_chunk):
            if output_chunk.size > 0:
                return

    def _need_next_row_group(self, state):
        if state.current_group_pos < 0:
            return True
        return state.group_offset >= self._current_row_group(state).num_rows


# ## Scan Node Table Operator
#
# ScanNodeTable is the high-level physical operator. It owns a scan state for
# the current table and asks a shared state object for more work when that scan
# state is exhausted.


class ScanNodeTableSharedState:
    def initialize(self, table):
        self.table = table
        self.next_committed_morsel = 0

        if table.is_arrow_table():
            # Arrow splits batches into smaller scan morsels.
            self.num_committed_morsels = table.num_scan_morsels()
        elif table.is_ice_disk_node_table():
            # Current ice-disk node scan uses Parquet row groups as morsels.
            self.num_committed_morsels = table.num_parquet_row_groups()
        else:
            # Native storage uses node groups.
            self.num_committed_morsels = table.num_committed_node_groups()

        table.initialize_scan_coordination()

    def next_morsel(self, scan_state):
        # In C++, this is mutex-protected because multiple workers can ask for
        # work concurrently.
        if self.table.is_arrow_table():
            return self.table.shared_scan_state.assign_next_arrow_morsel(scan_state)

        if self.next_committed_morsel < self.num_committed_morsels:
            scan_state.source = "COMMITTED"
            scan_state.node_group_idx = self.next_committed_morsel
            self.next_committed_morsel += 1
            return True

        scan_state.source = "NONE"
        return False


class ScanNodeTableOperator:
    def get_next_tuples(self):
        while self.current_table_idx < len(self.tables):
            table = self.tables[self.current_table_idx]

            while table.scan(self.scan_state):
                if self.scan_state.output_size > 0:
                    return self.scan_state.output_chunk

            if self.shared_state.next_morsel(self.scan_state):
                table.init_scan_state(self.scan_state)
            else:
                self.current_table_idx += 1
                self.init_next_table_if_any()

        return None


# ## Ice-Disk Node Scan
#
# Ice-disk node scan assigns one Parquet row group to a scan state. Each call to
# `scan_internal()` emits one Parquet execution vector and advances the Parquet
# scan state. The operator calls back until the assigned row group is exhausted.


class IceDiskNodeTable:
    def init_scan_state(self, scan_state):
        scan_state.scan_completed = False

        if not scan_state.initialized:
            scan_state.parquet_reader = ParquetReader(self.parquet_file)
            scan_state.parquet_scan_state = ParquetReaderScanState()
            scan_state.initialized = True

        row_group = self.shared_state.get_next_row_group()
        if row_group is None:
            scan_state.scan_completed = True
            scan_state.parquet_reader.initialize_scan(
                scan_state.parquet_scan_state,
                row_groups_to_read=[],
            )
            return

        scan_state.node_group_idx = row_group
        scan_state.parquet_reader.initialize_scan(
            scan_state.parquet_scan_state,
            row_groups_to_read=[row_group],
        )

    def scan_internal(self, scan_state):
        if scan_state.scan_completed:
            return False

        parquet_chunk = DataChunk(columns=self.parquet_columns)
        scan_state.parquet_reader.scan(scan_state.parquet_scan_state, parquet_chunk)

        if parquet_chunk.size == 0:
            scan_state.scan_completed = True
            return False

        global_start_offset = self.compute_global_start_offset(
            parquet_scan_state=scan_state.parquet_scan_state,
            rows_in_current_chunk=parquet_chunk.size,
        )

        output_to_parquet_column = self.map_output_columns_to_parquet_columns(
            scan_state.requested_column_ids,
        )

        for output_col, parquet_col in output_to_parquet_column.items():
            if parquet_col is None:
                scan_state.output_vectors[output_col].set_all_null(parquet_chunk.size)
            else:
                scan_state.output_vectors[output_col].copy_from(
                    parquet_chunk.vectors[parquet_col],
                    count=parquet_chunk.size,
                )

        for i in range(parquet_chunk.size):
            scan_state.node_id_vector[i] = NodeID(
                table_id=self.table_id,
                offset=global_start_offset + i,
            )

        scan_state.output_size = parquet_chunk.size
        return True


# ## Ice-Disk Relationship Scan
#
# Relationship scans are driven by a vector of bound input nodes. The rel table
# scan finds matching neighbors for those bound nodes and emits relationships
# for one active bound node at a time.
#
# CSR layout:
# - `indptr[source_offset]..indptr[source_offset + 1]` gives the edge row range.
# - `indices_*.parquet` stores destination offsets and edge properties.
#
# Flat layout:
# - one Parquet row per edge with source offset, target offset, and properties.


class IceDiskRelTableScanState:
    def __init__(self):
        self.parquet_scan_state = ParquetReaderScanState()
        self.cached_batch_data = None
        self.current_batch_start_offset = 0
        self.current_local_row_idx = 0
        self.bound_node_offsets = {}


class IceDiskRelTable:
    def init_scan_state(self, scan_state, bound_node_vector, direction):
        scan_state.direction = direction
        scan_state.bound_node_offsets = {
            node_id.offset: sel_pos
            for sel_pos, node_id in enumerate(bound_node_vector)
        }

        scan_state.cached_batch_data = None
        scan_state.current_batch_start_offset = 0
        scan_state.current_local_row_idx = 0

        self.indices_reader.initialize_scan(
            scan_state.parquet_scan_state,
            row_groups_to_read=list(range(self.indices_reader.num_row_groups())),
        )

        if self.layout == "CSR":
            self.load_indptr_data_once()

    def scan_internal(self, scan_state):
        if self.layout == "FLAT":
            return self.scan_flat(scan_state)
        return self.scan_csr(scan_state)

    def scan_csr(self, scan_state):
        return self._scan_edges(scan_state, row_to_offsets=self._csr_row_to_offsets)

    def scan_flat(self, scan_state):
        return self._scan_edges(scan_state, row_to_offsets=self._flat_row_to_offsets)

    def _scan_edges(self, scan_state, row_to_offsets):
        if not scan_state.bound_node_offsets:
            return False

        output_count = 0
        active_bound_offset = None
        active_bound_sel_pos = None

        while output_count < DEFAULT_VECTOR_CAPACITY:
            if self._need_next_cached_batch(scan_state):
                self._load_next_indices_batch(scan_state)

            if scan_state.cached_batch_data.size == 0:
                break

            while (
                scan_state.current_local_row_idx < scan_state.cached_batch_data.size
                and output_count < DEFAULT_VECTOR_CAPACITY
            ):
                local_row = scan_state.current_local_row_idx
                global_edge_row = scan_state.current_batch_start_offset + local_row

                source_offset, target_offset = row_to_offsets(
                    scan_state.cached_batch_data,
                    local_row,
                    global_edge_row,
                )

                if scan_state.direction == "FWD":
                    bound_offset = source_offset
                    neighbor_offset = target_offset
                    neighbor_table_id = self.to_table_id
                else:
                    bound_offset = target_offset
                    neighbor_offset = source_offset
                    neighbor_table_id = self.from_table_id

                bound_sel_pos = scan_state.bound_node_offsets.get(bound_offset)
                if bound_sel_pos is None:
                    scan_state.current_local_row_idx += 1
                    continue

                if active_bound_offset is None:
                    active_bound_offset = bound_offset
                    active_bound_sel_pos = bound_sel_pos
                elif bound_offset != active_bound_offset:
                    return self._finish_rel_output(
                        scan_state,
                        output_count,
                        active_bound_sel_pos,
                    )

                scan_state.output_vectors["neighbor"][output_count] = NodeID(
                    table_id=neighbor_table_id,
                    offset=neighbor_offset,
                )

                self._copy_edge_properties(
                    from_batch=scan_state.cached_batch_data,
                    input_row=local_row,
                    to_vectors=scan_state.output_vectors,
                    output_row=output_count,
                    rel_id_offset=global_edge_row,
                )

                output_count += 1
                scan_state.current_local_row_idx += 1

        return self._finish_rel_output(scan_state, output_count, active_bound_sel_pos)

    def _need_next_cached_batch(self, scan_state):
        return (
            scan_state.cached_batch_data is None
            or scan_state.current_local_row_idx == scan_state.cached_batch_data.size
        )

    def _load_next_indices_batch(self, scan_state):
        scan_state.current_batch_start_offset += scan_state.current_local_row_idx
        scan_state.current_local_row_idx = 0

        batch = DataChunk(columns=self.indices_columns)
        self.indices_reader.scan(scan_state.parquet_scan_state, batch)
        scan_state.cached_batch_data = batch

    def _finish_rel_output(self, scan_state, output_count, active_bound_sel_pos):
        if output_count == 0:
            scan_state.output_size = 0
            return False

        scan_state.output_size = output_count
        scan_state.set_node_id_vector_to_flat(active_bound_sel_pos)
        return True

    def _csr_row_to_offsets(self, batch, local_row, global_edge_row):
        source_offset = self.find_source_node_for_edge_row(global_edge_row)
        target_offset = batch["target"][local_row]
        return source_offset, target_offset

    def _flat_row_to_offsets(self, batch, local_row, global_edge_row):
        return batch["source"][local_row], batch["target"][local_row]


# ## Summary
#
# - Parquet reader calls are vector-sized, not row-group-sized.
# - ScanNodeTable asks shared state for morsels and reinitializes table scan
#   state for each assigned morsel.
# - Ice-disk node morsels are Parquet row groups, consumed over repeated
#   2048-row scan calls.
# - Ice-disk relationship scans read edge Parquet files in 2048-row cached
#   batches and emit relationships for one active bound node at a time.
#
# ## Key Mental Model
#
# ```python
# # Node scans:
# #   morsel = native node group / Arrow morsel / ice-disk Parquet row group
# #   scan_internal() = one output vector at a time
#
# # Parquet reader:
# #   initialize_scan(row_groups)
# #   scan() returns at most 2048 rows
# #   repeat scan() until empty to consume assigned row groups
#
# # Rel scans:
# #   input is a vector of bound nodes
# #   scan rel table finds neighbors for those bound nodes
# #   emits relationships for one active bound node at a time
# #   reads relationship parquet files in 2048-row chunks
# ```
