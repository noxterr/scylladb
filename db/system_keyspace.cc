/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Modified by ScyllaDB
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <boost/range/algorithm_ext/push_back.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/map.hpp>

#include "system_keyspace.hh"
#include "types.hh"
#include "service/storage_service.hh"
#include "service/storage_proxy.hh"
#include "service/client_state.hh"
#include "service/query_state.hh"
#include "cql3/query_options.hh"
#include "cql3/query_processor.hh"
#include "cql3/untyped_result_set.hh"
#include "utils/fb_utilities.hh"
#include "utils/hash.hh"
#include "dht/i_partitioner.hh"
#include "version.hh"
#include "thrift/server.hh"
#include "exceptions/exceptions.hh"
#include "cql3/query_processor.hh"
#include "query_context.hh"
#include "partition_slice_builder.hh"
#include "db/config.hh"
#include "system_keyspace_view_types.hh"
#include "schema_builder.hh"
#include "hashers.hh"
#include "release.hh"
#include "log.hh"
#include "serializer.hh"
#include <seastar/core/enum.hh>
#include <seastar/net/inet_address.hh>
#include <index/secondary_index.hh>
#include "service/storage_proxy.hh"
#include "message/messaging_service.hh"
#include "mutation_query.hh"
#include "db/size_estimates_virtual_reader.hh"
#include "db/timeout_clock.hh"
#include "sstables/sstables.hh"
#include "db/view/build_progress_virtual_reader.hh"
#include "db/schema_tables.hh"
#include "index/built_indexes_virtual_reader.hh"

using days = std::chrono::duration<int, std::ratio<24 * 3600>>;

namespace db {

std::unique_ptr<query_context> qctx = {};

namespace system_keyspace {

static logging::logger slogger("system_keyspace");
static const api::timestamp_type creation_timestamp = api::new_timestamp();

api::timestamp_type schema_creation_timestamp() {
    return creation_timestamp;
}

// Increase whenever changing schema of any system table.
// FIXME: Make automatic by calculating from schema structure.
static const uint16_t version_sequence_number = 1;

table_schema_version generate_schema_version(utils::UUID table_id) {
    md5_hasher h;
    feed_hash(h, table_id);
    feed_hash(h, version_sequence_number);
    return utils::UUID_gen::get_name_UUID(h.finalize());
}

// Currently, the type variables (uuid_type, etc.) are thread-local reference-
// counted shared pointers. This forces us to also make the built in schemas
// below thread-local as well.
// We return schema_ptr, not schema&, because that's the "tradition" in our
// other code.
// We hide the thread_local variable inside a function, because if we later
// we remove the thread_local, we'll start having initialization order
// problems (we need the type variables to be constructed first), and using
// functions will solve this problem. So we use functions right now.


schema_ptr hints() {
    static thread_local auto hints = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, HINTS), NAME, HINTS,
        // partition key
        {{"target_id", uuid_type}},
        // clustering key
        {{"hint_id", timeuuid_type}, {"message_version", int32_type}},
        // regular columns
        {{"mutation", bytes_type}},
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "hints awaiting delivery"
       )));
       builder.set_gc_grace_seconds(0);
       builder.set_compaction_strategy_options({{ "enabled", "false" }});
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build(schema_builder::compact_storage::yes);
    }();
    return hints;
}

schema_ptr batchlog() {
    static thread_local auto batchlog = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, BATCHLOG), NAME, BATCHLOG,
        // partition key
        {{"id", uuid_type}},
        // clustering key
        {},
        // regular columns
        {{"data", bytes_type}, {"version", int32_type}, {"written_at", timestamp_type}},
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "batches awaiting replay"
        // FIXME: the original Java code also had:
        // operations on resulting CFMetaData:
        //    .compactionStrategyOptions(Collections.singletonMap("min_threshold", "2"))
       )));
       builder.set_gc_grace_seconds(0);
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build(schema_builder::compact_storage::no);
    }();
    return batchlog;
}

/*static*/ schema_ptr paxos() {
    static thread_local auto paxos = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, PAXOS), NAME, PAXOS,
        // partition key
        {{"row_key", bytes_type}},
        // clustering key
        {{"cf_id", uuid_type}},
        // regular columns
        {{"in_progress_ballot", timeuuid_type}, {"most_recent_commit", bytes_type}, {"most_recent_commit_at", timeuuid_type}, {"proposal", bytes_type}, {"proposal_ballot", timeuuid_type}},
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "in-progress paxos proposals"
        // FIXME: the original Java code also had:
        // operations on resulting CFMetaData:
        //    .compactionStrategyClass(LeveledCompactionStrategy.class);
       )));
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build(schema_builder::compact_storage::no);
    }();
    return paxos;
}

schema_ptr built_indexes() {
    static thread_local auto built_indexes = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, BUILT_INDEXES), NAME, BUILT_INDEXES,
        // partition key
        {{"table_name", utf8_type}}, // table_name here is the name of the keyspace - don't be fooled
        // clustering key
        {{"index_name", utf8_type}},
        // regular columns
        {},
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "built column indexes"
       )));
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build(schema_builder::compact_storage::yes);
    }();
    return built_indexes;
}

/*static*/ schema_ptr local() {
    static thread_local auto local = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, LOCAL), NAME, LOCAL,
        // partition key
        {{"key", utf8_type}},
        // clustering key
        {},
        // regular columns
        {
                {"bootstrapped", utf8_type},
                {"cluster_name", utf8_type},
                {"cql_version", utf8_type},
                {"data_center", utf8_type},
                {"gossip_generation", int32_type},
                {"host_id", uuid_type},
                {"native_protocol_version", utf8_type},
                {"partitioner", utf8_type},
                {"rack", utf8_type},
                {"release_version", utf8_type},
                {"schema_version", uuid_type},
                {"thrift_version", utf8_type},
                {"tokens", set_type_impl::get_instance(utf8_type, true)},
                {"truncated_at", map_type_impl::get_instance(uuid_type, bytes_type, true)},
                // The following 3 columns are only present up until 2.1.8 tables
                {"rpc_address", inet_addr_type},
                {"broadcast_address", inet_addr_type},
                {"listen_address", inet_addr_type},
                {"supported_features", utf8_type},
                {"scylla_cpu_sharding_algorithm", utf8_type},
                {"scylla_nr_shards", int32_type},
                {"scylla_msb_ignore", int32_type},

        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "information about the local node"
       )));
       builder.with_version(generate_schema_version(builder.uuid()));
       builder.remove_column("scylla_cpu_sharding_algorithm");
       builder.remove_column("scylla_nr_shards");
       builder.remove_column("scylla_msb_ignore");
       return builder.build(schema_builder::compact_storage::no);
    }();
    return local;
}

/*static*/ schema_ptr peers() {
    static thread_local auto peers = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, PEERS), NAME, PEERS,
        // partition key
        {{"peer", inet_addr_type}},
        // clustering key
        {},
        // regular columns
        {
                {"data_center", utf8_type},
                {"host_id", uuid_type},
                {"preferred_ip", inet_addr_type},
                {"rack", utf8_type},
                {"release_version", utf8_type},
                {"rpc_address", inet_addr_type},
                {"schema_version", uuid_type},
                {"tokens", set_type_impl::get_instance(utf8_type, true)},
                {"supported_features", utf8_type},
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "information about known peers in the cluster"
       )));
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build(schema_builder::compact_storage::no);
    }();
    return peers;
}

/*static*/ schema_ptr peer_events() {
    static thread_local auto peer_events = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, PEER_EVENTS), NAME, PEER_EVENTS,
        // partition key
        {{"peer", inet_addr_type}},
        // clustering key
        {},
        // regular columns
        {
            {"hints_dropped", map_type_impl::get_instance(uuid_type, int32_type, true)},
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "events related to peers"
       )));
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build(schema_builder::compact_storage::no);
    }();
    return peer_events;
}

/*static*/ schema_ptr range_xfers() {
    static thread_local auto range_xfers = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, RANGE_XFERS), NAME, RANGE_XFERS,
        // partition key
        {{"token_bytes", bytes_type}},
        // clustering key
        {},
        // regular columns
        {{"requested_at", timestamp_type}},
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "ranges requested for transfer"
       )));
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build(schema_builder::compact_storage::no);
    }();
    return range_xfers;
}

/*static*/ schema_ptr compactions_in_progress() {
    static thread_local auto compactions_in_progress = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, COMPACTIONS_IN_PROGRESS), NAME, COMPACTIONS_IN_PROGRESS,
        // partition key
        {{"id", uuid_type}},
        // clustering key
        {},
        // regular columns
        {
            {"columnfamily_name", utf8_type},
            {"inputs", set_type_impl::get_instance(int32_type, true)},
            {"keyspace_name", utf8_type},
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "unfinished compactions"
        )));
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build(schema_builder::compact_storage::no);
    }();
    return compactions_in_progress;
}

/*static*/ schema_ptr compaction_history() {
    static thread_local auto compaction_history = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, COMPACTION_HISTORY), NAME, COMPACTION_HISTORY,
        // partition key
        {{"id", uuid_type}},
        // clustering key
        {},
        // regular columns
        {
            {"bytes_in", long_type},
            {"bytes_out", long_type},
            {"columnfamily_name", utf8_type},
            {"compacted_at", timestamp_type},
            {"keyspace_name", utf8_type},
            {"rows_merged", map_type_impl::get_instance(int32_type, long_type, true)},
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "week-long compaction history"
        )));
        builder.set_default_time_to_live(std::chrono::duration_cast<std::chrono::seconds>(days(7)));
        builder.with_version(generate_schema_version(builder.uuid()));
        return builder.build(schema_builder::compact_storage::no);
    }();
    return compaction_history;
}

/*static*/ schema_ptr sstable_activity() {
    static thread_local auto sstable_activity = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, SSTABLE_ACTIVITY), NAME, SSTABLE_ACTIVITY,
        // partition key
        {
            {"keyspace_name", utf8_type},
            {"columnfamily_name", utf8_type},
            {"generation", int32_type},
        },
        // clustering key
        {},
        // regular columns
        {
            {"rate_120m", double_type},
            {"rate_15m", double_type},
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "historic sstable read rates"
       )));
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build(schema_builder::compact_storage::no);
    }();
    return sstable_activity;
}

schema_ptr size_estimates() {
    static thread_local auto size_estimates = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, SIZE_ESTIMATES), NAME, SIZE_ESTIMATES,
            // partition key
            {{"keyspace_name", utf8_type}},
            // clustering key
            {{"table_name", utf8_type}, {"range_start", utf8_type}, {"range_end", utf8_type}},
            // regular columns
            {
                {"mean_partition_size", long_type},
                {"partitions_count", long_type},
            },
            // static columns
            {},
            // regular column name type
            utf8_type,
            // comment
            "per-table primary range size estimates"
            )));
        builder.set_gc_grace_seconds(0);
        builder.with_version(generate_schema_version(builder.uuid()));
        return builder.build(schema_builder::compact_storage::no);
    }();
    return size_estimates;
}

/*static*/ schema_ptr large_partitions() {
    static thread_local auto large_partitions = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, LARGE_PARTITIONS), NAME, LARGE_PARTITIONS,
        // partition key
        {{"keyspace_name", utf8_type}, {"table_name", utf8_type}},
        // clustering key
        {
            {"sstable_name", utf8_type},
            {"partition_size", reversed_type_impl::get_instance(long_type)},
            {"partition_key", utf8_type}
        }, // CLUSTERING ORDER BY (partition_size DESC)
        // regular columns
        {{"compaction_time", timestamp_type}},
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "partitions larger than specified threshold"
        )));
        builder.with_version(generate_schema_version(builder.uuid()));
        return builder.build(schema_builder::compact_storage::no);
    }();
    return large_partitions;
}

static schema_ptr large_rows() {
    static thread_local auto large_rows = [] {
        auto id = generate_legacy_id(NAME, LARGE_ROWS);
        return schema_builder(NAME, LARGE_ROWS, std::optional(id))
                .with_column("keyspace_name", utf8_type, column_kind::partition_key)
                .with_column("table_name", utf8_type, column_kind::partition_key)
                .with_column("sstable_name", utf8_type, column_kind::clustering_key)
                // We want the large rows first, so use reversed_type_impl
                .with_column("row_size", reversed_type_impl::get_instance(long_type), column_kind::clustering_key)
                .with_column("partition_key", utf8_type, column_kind::clustering_key)
                .with_column("clustering_key", utf8_type, column_kind::clustering_key)
                .with_column("compaction_time", timestamp_type)
                .set_comment("rows larger than specified threshold")
                .with_version(generate_schema_version(id))
                .build();
    }();
    return large_rows;
}

static schema_ptr large_cells() {
    static thread_local auto large_cells = [] {
        auto id = generate_legacy_id(NAME, LARGE_CELLS);
        return schema_builder(NAME, LARGE_CELLS, id)
                .with_column("keyspace_name", utf8_type, column_kind::partition_key)
                .with_column("table_name", utf8_type, column_kind::partition_key)
                .with_column("sstable_name", utf8_type, column_kind::clustering_key)
                // We want the larger cells first, so use reversed_type_impl
                .with_column("cell_size", reversed_type_impl::get_instance(long_type), column_kind::clustering_key)
                .with_column("partition_key", utf8_type, column_kind::clustering_key)
                .with_column("clustering_key", utf8_type, column_kind::clustering_key)
                .with_column("column_name", utf8_type, column_kind::clustering_key)
                .with_column("compaction_time", timestamp_type)
                .set_comment("cells larger than specified threshold")
                .with_version(generate_schema_version(id))
                .build();
    }();
    return large_cells;
}

/*static*/ schema_ptr scylla_local() {
    static thread_local auto scylla_local = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, SCYLLA_LOCAL), NAME, SCYLLA_LOCAL,
        // partition key
        {{"key", utf8_type}},
        // clustering key
        {},
        // regular columns
        {
                {"value", utf8_type},
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "Scylla specific information about the local node"
       )));
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build(schema_builder::compact_storage::no);
    }();
    return scylla_local;
}

namespace v3 {

schema_ptr batches() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, BATCHES), NAME, BATCHES,
        // partition key
        {{"id", timeuuid_type}},
        // clustering key
        {},
        // regular columns
        {{"mutations", list_type_impl::get_instance(bytes_type, true)}, {"version", int32_type}},
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "batches awaiting replay"
       )));
       builder.set_gc_grace_seconds(0);
       // FIXME: the original Java code also had:
       //.copy(new LocalPartitioner(TimeUUIDType.instance))
       builder.set_gc_grace_seconds(0);
       builder.set_compaction_strategy(sstables::compaction_strategy_type::size_tiered);
       builder.set_compaction_strategy_options({{"min_threshold", "2"}});
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build(schema_builder::compact_storage::no);
    }();
    return schema;
}

schema_ptr paxos() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, PAXOS), NAME, PAXOS,
        // partition key
        {{"row_key", bytes_type}},
        // clustering key
        {{"cf_id", uuid_type}},
        // regular columns
        {{"in_progress_ballot", timeuuid_type},
         {"most_recent_commit", bytes_type},
         {"most_recent_commit_at", timeuuid_type},
         {"most_recent_commit_version", int32_type},
         {"proposal", timeuuid_type},
         {"proposal_version", int32_type}
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "in-progress paxos proposals"
       )));
       builder.set_compaction_strategy(sstables::compaction_strategy_type::leveled);
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build();
    }();
    return schema;
}

schema_ptr built_indexes() {
    // identical to ours, but ours otoh is a mix-in of the 3.x series cassandra one
    return db::system_keyspace::built_indexes();
}

schema_ptr local() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, LOCAL), NAME, LOCAL,
        // partition key
        {{"key", utf8_type}},
        // clustering key
        {},
        // regular columns
        {
                {"bootstrapped", utf8_type},
                {"broadcast_address", inet_addr_type},
                {"cluster_name", utf8_type},
                {"cql_version", utf8_type},
                {"data_center", utf8_type},
                {"gossip_generation", int32_type},
                {"host_id", uuid_type},
                {"listen_address", inet_addr_type},
                {"native_protocol_version", utf8_type},
                {"partitioner", utf8_type},
                {"rack", utf8_type},
                {"release_version", utf8_type},
                {"rpc_address", inet_addr_type},
                {"schema_version", uuid_type},
                {"thrift_version", utf8_type},
                {"tokens", set_type_impl::get_instance(utf8_type, true)},
                {"truncated_at", map_type_impl::get_instance(uuid_type, bytes_type, true)},
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "information about the local node"
       )));
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build(schema_builder::compact_storage::no);
    }();
    return schema;
}

schema_ptr truncated() {
    static thread_local auto local = [] {
        schema_builder builder(make_lw_shared(schema(generate_legacy_id(NAME, TRUNCATED), NAME, TRUNCATED,
        // partition key
        {{"table_uuid", uuid_type}},
        // clustering key
        {{"shard", int32_type}},
        // regular columns
        {
                {"position", int32_type},
                {"segment_id", long_type}
        },
        // static columns
        {
                {"truncated_at", timestamp_type},
        },
        // regular column name type
        utf8_type,
        // comment
        "information about table truncation"
       )));
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build(schema_builder::compact_storage::no);
    }();
    return local;
}

schema_ptr peers() {
    // identical
    return db::system_keyspace::peers();
}

schema_ptr peer_events() {
    // identical
    return db::system_keyspace::peer_events();
}

schema_ptr range_xfers() {
    // identical
    return db::system_keyspace::range_xfers();
}

schema_ptr compaction_history() {
    // identical
    return db::system_keyspace::compaction_history();
}

schema_ptr sstable_activity() {
    // identical
    return db::system_keyspace::sstable_activity();
}

schema_ptr size_estimates() {
    // identical
    return db::system_keyspace::size_estimates();
}

schema_ptr large_partitions() {
    // identical
    return db::system_keyspace::large_partitions();
}

schema_ptr scylla_local() {
    // identical
    return db::system_keyspace::scylla_local();
}

schema_ptr available_ranges() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, AVAILABLE_RANGES), NAME, AVAILABLE_RANGES,
        // partition key
        {{"keyspace_name", utf8_type}},
        // clustering key
        {},
        // regular columns
        {{"ranges", set_type_impl::get_instance(bytes_type, true)}},
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "available keyspace/ranges during bootstrap/replace that are ready to be served"
       )));
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build();
    }();
    return schema;
}

schema_ptr views_builds_in_progress() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, VIEWS_BUILDS_IN_PROGRESS), NAME, VIEWS_BUILDS_IN_PROGRESS,
        // partition key
        {{"keyspace_name", utf8_type}},
        // clustering key
        {{"view_name", utf8_type}},
        // regular columns
        {{"last_token", utf8_type}, {"generation_number", int32_type}},
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "views builds current progress"
       )));
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build();
    }();
    return schema;
}

schema_ptr built_views() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, BUILT_VIEWS), NAME, BUILT_VIEWS,
        // partition key
        {{"keyspace_name", utf8_type}},
        // clustering key
        {{"view_name", utf8_type}},
        // regular columns
        {},
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "built views"
       )));
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build();
    }();
    return schema;
}

schema_ptr scylla_views_builds_in_progress() {
    static thread_local auto schema = [] {
        auto id = generate_legacy_id(NAME, SCYLLA_VIEWS_BUILDS_IN_PROGRESS);
        return schema_builder(NAME, SCYLLA_VIEWS_BUILDS_IN_PROGRESS, std::make_optional(id))
                .with_column("keyspace_name", utf8_type, column_kind::partition_key)
                .with_column("view_name", utf8_type, column_kind::clustering_key)
                .with_column("cpu_id", int32_type, column_kind::clustering_key)
                .with_column("next_token", utf8_type)
                .with_column("generation_number", int32_type)
                .with_column("first_token", utf8_type)
                .with_version(generate_schema_version(id))
                .build();
    }();
    return schema;
}

} //</v3>

namespace legacy {

schema_ptr hints() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, HINTS), NAME, HINTS,
        // partition key
        {{"target_id", uuid_type}},
        // clustering key
        {{"hint_id", timeuuid_type}, {"message_version", int32_type}},
        // regular columns
        {{"mutation", bytes_type}},
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "*DEPRECATED* hints awaiting delivery"
       )));
       builder.set_gc_grace_seconds(0);
       builder.set_compaction_strategy(sstables::compaction_strategy_type::size_tiered);
       builder.set_compaction_strategy_options({{"enabled", "false"}});
       builder.with_version(generate_schema_version(builder.uuid()));
       builder.with(schema_builder::compact_storage::yes);
       return builder.build();
    }();
    return schema;
}

schema_ptr batchlog() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, BATCHLOG), NAME, BATCHLOG,
        // partition key
        {{"id", uuid_type}},
        // clustering key
        {},
        // regular columns
        {{"data", bytes_type}, {"version", int32_type}, {"written_at", timestamp_type}},
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "*DEPRECATED* batchlog entries"
       )));
       builder.set_gc_grace_seconds(0);
       builder.set_compaction_strategy(sstables::compaction_strategy_type::size_tiered);
       builder.set_compaction_strategy_options({{"min_threshold", "2"}});
       builder.with(schema_builder::compact_storage::no);
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build();
    }();
    return schema;
}

static constexpr auto schema_gc_grace = std::chrono::duration_cast<std::chrono::seconds>(days(7)).count();

schema_ptr keyspaces() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, KEYSPACES), NAME, KEYSPACES,
        // partition key
        {{"keyspace_name", utf8_type}},
        // clustering key
        {},
        // regular columns
        {
         {"durable_writes", boolean_type},
         {"strategy_class", utf8_type},
         {"strategy_options", utf8_type}
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "*DEPRECATED* keyspace definitions"
       )));
       builder.set_gc_grace_seconds(schema_gc_grace);
       builder.with(schema_builder::compact_storage::yes);
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build();
    }();
    return schema;
}

schema_ptr column_families() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, COLUMNFAMILIES), NAME, COLUMNFAMILIES,
        // partition key
        {{"keyspace_name", utf8_type}},
        // clustering key
        {{"columnfamily_name", utf8_type}},
        // regular columns
        {
         {"bloom_filter_fp_chance", double_type},
         {"caching", utf8_type},
         {"cf_id", uuid_type},
         {"comment", utf8_type},
         {"compaction_strategy_class", utf8_type},
         {"compaction_strategy_options", utf8_type},
         {"comparator", utf8_type},
         {"compression_parameters", utf8_type},
         {"default_time_to_live", int32_type},
         {"default_validator", utf8_type},
         {"dropped_columns",  map_type_impl::get_instance(utf8_type, long_type, true)},
         {"gc_grace_seconds", int32_type},
         {"is_dense", boolean_type},
         {"key_validator", utf8_type},
         {"local_read_repair_chance", double_type},
         {"max_compaction_threshold", int32_type},
         {"max_index_interval", int32_type},
         {"memtable_flush_period_in_ms", int32_type},
         {"min_compaction_threshold", int32_type},
         {"min_index_interval", int32_type},
         {"read_repair_chance", double_type},
         {"speculative_retry", utf8_type},
         {"subcomparator", utf8_type},
         {"type", utf8_type},
         // The following 4 columns are only present up until 2.1.8 tables
         {"key_aliases", utf8_type},
         {"value_alias", utf8_type},
         {"column_aliases", utf8_type},
         {"index_interval", int32_type},},
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "*DEPRECATED* table definitions"
       )));
       builder.set_gc_grace_seconds(schema_gc_grace);
       builder.with(schema_builder::compact_storage::no);
       builder.with_version(generate_schema_version(builder.uuid()));
       return builder.build();
    }();
    return schema;
}

schema_ptr columns() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, COLUMNS), NAME, COLUMNS,
        // partition key
        {{"keyspace_name", utf8_type}},
        // clustering key
        {{"columnfamily_name", utf8_type}, {"column_name", utf8_type}},
        // regular columns
        {
            {"component_index", int32_type},
            {"index_name", utf8_type},
            {"index_options", utf8_type},
            {"index_type", utf8_type},
            {"type", utf8_type},
            {"validator", utf8_type},
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "column definitions"
        )));
        builder.set_gc_grace_seconds(schema_gc_grace);
        builder.with(schema_builder::compact_storage::no);
        builder.with_version(generate_schema_version(builder.uuid()));
        return builder.build();
    }();
    return schema;
}

schema_ptr triggers() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, TRIGGERS), NAME, TRIGGERS,
        // partition key
        {{"keyspace_name", utf8_type}},
        // clustering key
        {{"columnfamily_name", utf8_type}, {"trigger_name", utf8_type}},
        // regular columns
        {
            {"trigger_options",  map_type_impl::get_instance(utf8_type, utf8_type, true)},
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "trigger definitions"
        )));
        builder.set_gc_grace_seconds(schema_gc_grace);
        builder.with(schema_builder::compact_storage::no);
        builder.with_version(generate_schema_version(builder.uuid()));
        return builder.build();
    }();
    return schema;
}

schema_ptr usertypes() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, USERTYPES), NAME, USERTYPES,
        // partition key
        {{"keyspace_name", utf8_type}},
        // clustering key
        {{"type_name", utf8_type}},
        // regular columns
        {
            {"field_names", list_type_impl::get_instance(utf8_type, true)},
            {"field_types", list_type_impl::get_instance(utf8_type, true)},
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "user defined type definitions"
        )));
        builder.set_gc_grace_seconds(schema_gc_grace);
        builder.with(schema_builder::compact_storage::no);
        builder.with_version(generate_schema_version(builder.uuid()));
        return builder.build();
    }();
    return schema;
}

schema_ptr functions() {
    /**
     * Note: we have our own "legacy" version of this table (in schema_tables),
     * but it is (afaik) not used, and differs slightly from the origin one.
     * This is based on the origin schema, since we're more likely to encounter
     * installations of that to migrate, rather than our own (if we dont use the table).
     */
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, FUNCTIONS), NAME, FUNCTIONS,
        // partition key
        {{"keyspace_name", utf8_type}},
        // clustering key
        {{"function_name", utf8_type},{"signature", list_type_impl::get_instance(utf8_type, false)}},
        // regular columns
        {
            {"argument_names", list_type_impl::get_instance(utf8_type, true)},
            {"argument_types", list_type_impl::get_instance(utf8_type, true)},
            {"body", utf8_type},
            {"language", utf8_type},
            {"return_type", utf8_type},
            {"called_on_null_input", boolean_type},
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "*DEPRECATED* user defined type definitions"
        )));
        builder.set_gc_grace_seconds(schema_gc_grace);
        builder.with(schema_builder::compact_storage::no);
        builder.with_version(generate_schema_version(builder.uuid()));
        return builder.build();
    }();
    return schema;
}

schema_ptr aggregates() {
    static thread_local auto schema = [] {
        schema_builder builder(make_lw_shared(::schema(generate_legacy_id(NAME, AGGREGATES), NAME, AGGREGATES,
        // partition key
        {{"keyspace_name", utf8_type}},
        // clustering key
        {{"aggregate_name", utf8_type},{"signature", list_type_impl::get_instance(utf8_type, false)}},
        // regular columns
        {
            {"argument_types", list_type_impl::get_instance(utf8_type, true)},
            {"final_func", utf8_type},
            {"initcond", bytes_type},
            {"return_type", utf8_type},
            {"state_func", utf8_type},
            {"state_type", utf8_type},
        },
        // static columns
        {},
        // regular column name type
        utf8_type,
        // comment
        "*DEPRECATED* user defined aggregate definition"
        )));
        builder.set_gc_grace_seconds(schema_gc_grace);
        builder.with(schema_builder::compact_storage::no);
        builder.with_version(generate_schema_version(builder.uuid()));
        return builder.build();
    }();
    return schema;
}

} //</legacy>

static future<> setup_version() {
    return gms::inet_address::lookup(qctx->db().get_config().rpc_address()).then([](gms::inet_address a) {
        sstring req = sprint("INSERT INTO system.%s (key, release_version, cql_version, thrift_version, native_protocol_version, data_center, rack, partitioner, rpc_address, broadcast_address, listen_address, supported_features) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
                        , db::system_keyspace::LOCAL);
        auto& snitch = locator::i_endpoint_snitch::get_local_snitch_ptr();

        return execute_cql(req, sstring(db::system_keyspace::LOCAL),
                             version::release(),
                             cql3::query_processor::CQL_VERSION,
                             ::cassandra::thrift_version,
                             to_sstring(cql_serialization_format::latest_version),
                             snitch->get_datacenter(utils::fb_utilities::get_broadcast_address()),
                             snitch->get_rack(utils::fb_utilities::get_broadcast_address()),
                             sstring(dht::global_partitioner().name()),
                             a.addr(),
                             utils::fb_utilities::get_broadcast_address().addr(),
                             netw::get_local_messaging_service().listen_address().addr(),
                             service::get_local_storage_service().get_config_supported_features()
        ).discard_result();
    });
}

future<> check_health();
future<> force_blocking_flush(sstring cfname);

// Changing the real load_dc_rack_info into a future would trigger a tidal wave of futurization that would spread
// even into simple string operations like get_rack() / get_dc(). We will cache those at startup, and then change
// our view of it every time we do updates on those values.
//
// The cache must be distributed, because the values themselves may not update atomically, so a shard reading that
// is different than the one that wrote, may see a corrupted value. invoke_on_all will be used to guarantee that all
// updates are propagated correctly.
struct local_cache {
    std::unordered_map<gms::inet_address, locator::endpoint_dc_rack> _cached_dc_rack_info;
    bootstrap_state _state;
    future<> stop() {
        return make_ready_future<>();
    }
};
static distributed<local_cache> _local_cache;

static future<> build_dc_rack_info() {
    return execute_cql(format("SELECT peer, data_center, rack from system.{}", PEERS)).then([] (::shared_ptr<cql3::untyped_result_set> msg) {
        return do_for_each(*msg, [] (auto& row) {
            net::inet_address peer = row.template get_as<net::inet_address>("peer");
            if (!row.has("data_center") || !row.has("rack")) {
                return make_ready_future<>();
            }
            gms::inet_address gms_addr(std::move(peer));
            sstring dc = row.template get_as<sstring>("data_center");
            sstring rack = row.template get_as<sstring>("rack");

            locator::endpoint_dc_rack  element = { dc, rack };
            return _local_cache.invoke_on_all([gms_addr = std::move(gms_addr), element = std::move(element)] (local_cache& lc) {
                lc._cached_dc_rack_info.emplace(gms_addr, element);
            });
        }).then([msg] {
            // Keep msg alive.
        });
    });
}

static future<> build_bootstrap_info() {
    sstring req = format("SELECT bootstrapped FROM system.{} WHERE key = ? ", LOCAL);
    return execute_cql(req, sstring(LOCAL)).then([] (auto msg) {
        static auto state_map = std::unordered_map<sstring, bootstrap_state>({
            { "NEEDS_BOOTSTRAP", bootstrap_state::NEEDS_BOOTSTRAP },
            { "COMPLETED", bootstrap_state::COMPLETED },
            { "IN_PROGRESS", bootstrap_state::IN_PROGRESS },
            { "DECOMMISSIONED", bootstrap_state::DECOMMISSIONED }
        });
        bootstrap_state state = bootstrap_state::NEEDS_BOOTSTRAP;

        if (!msg->empty() && msg->one().has("bootstrapped")) {
            state = state_map.at(msg->one().template get_as<sstring>("bootstrapped"));
        }
        return _local_cache.invoke_on_all([state] (local_cache& lc) {
            lc._state = state;
        });
    });
}

future<> init_local_cache() {
    return _local_cache.start().then([] {

        // Do not stop _local_cache here. See #2721.
        /*
        engine().at_exit([] {
            return _local_cache.stop();
        });
        */
    });
}

future<> deinit_local_cache() {
    return _local_cache.stop();
}

void minimal_setup(distributed<database>& db, distributed<cql3::query_processor>& qp) {
    qctx = std::make_unique<query_context>(db, qp);
}

future<> setup(distributed<database>& db,
               distributed<cql3::query_processor>& qp,
               distributed<service::storage_service>& ss) {
    minimal_setup(db, qp);
    return setup_version().then([&db] {
        return update_schema_version(db.local().get_version());
    }).then([] {
        return init_local_cache();
    }).then([] {
        return build_dc_rack_info();
    }).then([] {
        return build_bootstrap_info();
    }).then([] {
        return check_health();
    }).then([] {
        return db::schema_tables::save_system_keyspace_schema();
    }).then([] {
        // #2514 - make sure "system" is written to system_schema.keyspaces.
        return db::schema_tables::save_system_schema(NAME);
    }).then([] {
        return netw::get_messaging_service().invoke_on_all([] (auto& ms){
            return ms.init_local_preferred_ip_cache();
        });
    });
}

struct truncation_record {
    static constexpr uint32_t current_magic = 0x53435452; // 'S' 'C' 'T' 'R'

    uint32_t magic;
    std::vector<db::replay_position> positions;
    db_clock::time_point time_stamp;
};
}
}

#include "idl/replay_position.dist.hh"
#include "idl/truncation_record.dist.hh"
#include "serializer_impl.hh"
#include "idl/replay_position.dist.impl.hh"
#include "idl/truncation_record.dist.impl.hh"

namespace db {
namespace system_keyspace {

typedef utils::UUID truncation_key;
typedef std::unordered_map<truncation_key, truncation_record> truncation_map;

static constexpr uint8_t current_version = 1;
static bool need_legacy_truncation_records = true;
static thread_local shared_promise<> migration_complete;

future<> wait_for_truncation_record_migration_complete() {
   return migration_complete.get_shared_future();
}

/**
 * This method is used to remove information about truncation time for specified column family
 */
future<> remove_truncation_record(utils::UUID id) {
    sstring req = format("DELETE * from system.{} WHERE table_uuid = ?", TRUNCATED);
    return qctx->qp().execute_internal(req, {id}).discard_result().then([] {
        return force_blocking_flush(TRUNCATED);
    });
}

static future<truncation_record> get_truncation_record(utils::UUID cf_id) {
    sstring req = format("SELECT * from system.{} WHERE table_uuid = ?", TRUNCATED);
    return qctx->qp().execute_internal(req, {cf_id}).then([cf_id](::shared_ptr<cql3::untyped_result_set> rs) {
        truncation_record r{truncation_record::current_magic};

        for (const cql3::untyped_result_set_row& row : *rs) {
            auto shard = row.get_as<int32_t>("shard");
            auto ts = row.get_as<db_clock::time_point>("truncated_at");
            auto pos = row.get_as<int32_t>("position");
            auto id = row.get_as<int64_t>("segment_id");

            r.time_stamp = ts;
            r.positions.emplace_back(replay_position(shard, id, pos));
        }
        return make_ready_future<truncation_record>(std::move(r));
    });
}

future<> migrate_truncation_records() {
    sstring req = format("SELECT truncated_at FROM system.{} WHERE key = '{}'", LOCAL, LOCAL);
    return qctx->qp().execute_internal(req).then([](::shared_ptr<cql3::untyped_result_set> rs) {
        truncation_map tmp;
        if (!rs->empty() && rs->one().has("truncated_at")) {
            auto map = rs->one().get_map<utils::UUID, bytes>("truncated_at");
            for (auto& p : map) {
                auto uuid = p.first;
                auto buf = p.second;

                try {
                    truncation_record e;

                    if (buf.size() & 1) {
                        // new record.
                        if (buf[0] != current_version) {
                            slogger.warn("Found truncation record of unknown version {}. Ignoring.", int(buf[0]));
                            continue;
                        }
                        e = ser::deserialize_from_buffer(buf, boost::type<truncation_record>(), 1);
                        if (e.magic == truncation_record::current_magic) {
                            tmp[uuid] = e;
                            continue;
                        }
                    } else {
                        // old scylla records. (We hope)
                        // Read 64+64 bit RP:s, even though the
                        // struct (and official serial size) is 64+32.
                        data_input in(buf);

                        slogger.debug("Reading old type record");
                        while (in.avail() > sizeof(db_clock::rep)) {
                            auto id = in.read<uint64_t>();
                            auto pos = in.read<uint64_t>();
                            e.positions.emplace_back(id, position_type(pos));
                        }
                        if (in.avail() == sizeof(db_clock::rep)) {
                            e.time_stamp = db_clock::time_point(db_clock::duration(in.read<db_clock::rep>()));
                            tmp[uuid] = e;
                            continue;
                        }
                    }
                } catch (std::out_of_range &) {
                }
                // Trying to load an origin table.
                // This is useless to us, because the only usage for this
                // data is commit log and batch replay, and we cannot replay
                // either from origin anyway.
                slogger.warn("Error reading truncation record for {}. "
                                "Most likely this is data from a cassandra instance."
                                "Make sure you have cleared commit and batch logs before upgrading.",
                                uuid
                );
            }
        }

        auto i = tmp.begin();
        auto e = tmp.end();
        return parallel_for_each(i, e, [](const truncation_map::value_type& p) {
            const utils::UUID& uuid = p.first;
            const truncation_record& tr = p.second;
            return get_truncation_record(uuid).then([&](truncation_record new_record) {
                if (!new_record.positions.empty() && new_record.time_stamp >= tr.time_stamp) {
                    return make_ready_future<>();
                }
                return parallel_for_each(tr.positions, [&](replay_position rp) {
                    return save_truncation_record(uuid, tr.time_stamp, rp);
                });
            });
        }).then([tmp = std::move(tmp)] {
            auto& ss = service::get_local_storage_service();
            need_legacy_truncation_records = !ss.cluster_supports_truncation_table();

            if (need_legacy_truncation_records || !tmp.empty()) {
                ss.cluster_supports_truncation_table().when_enabled().then([] {
                    // this potentially races with a truncation, i.e. someone could be inserting into
                    // the legacy column while we delete it. But this is ok, it will just mean we have
                    // some unneeded data and will do a merge again next boot, but eventually we
                    // will remove the legacy data...
                    auto level = need_legacy_truncation_records ? seastar::log_level::info : seastar::log_level::debug;
                    slogger.log(level, "Got cluster agreement on truncation table feature. Removing legacy records.");
                    need_legacy_truncation_records = false;
                    sstring req = format("DELETE truncated_at from system.{} WHERE key = '{}'", LOCAL, LOCAL);
                    return qctx->qp().execute_internal(req).discard_result().then([level] {
                        slogger.log(level, "Legacy records deleted.");
                        return force_blocking_flush(LOCAL).then([] {
                            return smp::invoke_on_all([] {
                                migration_complete.set_value();
                            });
                        });
                    });
                });
            }
            return make_ready_future<>();
        });
    });
}

static future<> save_legacy_truncation_records(utils::UUID id, db_clock::time_point truncated_at, replay_positions positions) {
    truncation_record r;

    r.magic = truncation_record::current_magic;
    r.time_stamp = truncated_at;
    r.positions = std::move(positions);

    auto buf = ser::serialize_to_buffer<bytes>(r, sizeof(current_version));

    buf[0] = current_version;

    static_assert(sizeof(current_version) == 1, "using this as mark");
    assert(buf.size() & 1); // verify we've created an odd-numbered buffer

    map_type_impl::native_type tmp;
    tmp.emplace_back(id, data_value(buf));
    auto map_type = map_type_impl::get_instance(uuid_type, bytes_type, true);

    sstring req = format("UPDATE system.{} SET truncated_at = truncated_at + ? WHERE key = '{}'", LOCAL, LOCAL);
    return qctx->qp().execute_internal(req, {make_map_value(map_type, tmp)}).then([](auto rs) {
        return force_blocking_flush(LOCAL);
    });
}

static future<> maybe_save_legacy_truncation_record(utils::UUID id, db_clock::time_point truncated_at, db::replay_position rp) {
    if (!need_legacy_truncation_records) {
        return make_ready_future<>();
    }
    // TODO: this is horribly ineffective, we're doing a full flush of all system tables for all cores
    // once, for each core (calling us). But right now, redesigning so that calling here (or, rather,
    // save_truncation_records), is done from "somewhere higher, once per machine, not shard" is tricky.
    // Mainly because drop_tables also uses truncate. And is run per-core as well. Gah.
    return get_truncation_record(id).then([id, truncated_at, rp](truncation_record e) {
        auto i = std::find_if(e.positions.begin(), e.positions.end(), [rp](replay_position& p) {
            return p.shard_id() == rp.shard_id();
        });
        if (i == e.positions.end()) {
            e.positions.emplace_back(rp);
        } else {
            *i = rp;
        }
        return save_legacy_truncation_records(id, std::max(truncated_at, e.time_stamp), e.positions);
    });
}

future<> save_truncation_record(utils::UUID id, db_clock::time_point truncated_at, db::replay_position rp) {
    sstring req = format("INSERT INTO system.{} (table_uuid, shard, position, segment_id, truncated_at) VALUES(?,?,?,?,?)", TRUNCATED);
    return qctx->qp().execute_internal(req, {id, int32_t(rp.shard_id()), int32_t(rp.pos), int64_t(rp.base_id()), truncated_at}).discard_result().then([] {
        return force_blocking_flush(TRUNCATED);
    }).then([=] {
        return maybe_save_legacy_truncation_record(id, truncated_at, rp);
    });
}

future<> save_truncation_record(const column_family& cf, db_clock::time_point truncated_at, db::replay_position rp) {
    return save_truncation_record(cf.schema()->id(), truncated_at, rp);
}

future<db::replay_position> get_truncated_position(utils::UUID cf_id, uint32_t shard) {
    return get_truncated_position(std::move(cf_id)).then([shard](replay_positions positions) {
       for (auto& rp : positions) {
           if (shard == rp.shard_id()) {
               return make_ready_future<db::replay_position>(rp);
           }
       }
       return make_ready_future<db::replay_position>();
    });
}

future<replay_positions> get_truncated_position(utils::UUID cf_id) {
    return get_truncation_record(cf_id).then([](truncation_record e) {
        return make_ready_future<replay_positions>(e.positions);
    });
}

future<db_clock::time_point> get_truncated_at(utils::UUID cf_id) {
    return get_truncation_record(cf_id).then([](truncation_record e) {
        return make_ready_future<db_clock::time_point>(e.time_stamp);
    });
}

set_type_impl::native_type prepare_tokens(std::unordered_set<dht::token>& tokens) {
    set_type_impl::native_type tset;
    for (auto& t: tokens) {
        tset.push_back(dht::global_partitioner().to_sstring(t));
    }
    return tset;
}

std::unordered_set<dht::token> decode_tokens(set_type_impl::native_type& tokens) {
    std::unordered_set<dht::token> tset;
    for (auto& t: tokens) {
        auto str = value_cast<sstring>(t);
        assert(str == dht::global_partitioner().to_sstring(dht::global_partitioner().from_sstring(str)));
        tset.insert(dht::global_partitioner().from_sstring(str));
    }
    return tset;
}

/**
 * Record tokens being used by another node
 */
future<> update_tokens(gms::inet_address ep, std::unordered_set<dht::token> tokens)
{
    if (ep == utils::fb_utilities::get_broadcast_address()) {
        return remove_endpoint(ep);
    }

    sstring req = format("INSERT INTO system.{} (peer, tokens) VALUES (?, ?)", PEERS);
    auto set_type = set_type_impl::get_instance(utf8_type, true);
    return execute_cql(req, ep.addr(), make_set_value(set_type, prepare_tokens(tokens))).discard_result().then([] {
        return force_blocking_flush(PEERS);
    });
}

future<std::unordered_set<dht::token>> update_local_tokens(
    const std::unordered_set<dht::token> add_tokens,
    const std::unordered_set<dht::token> rm_tokens) {
    return get_saved_tokens().then([add_tokens = std::move(add_tokens), rm_tokens = std::move(rm_tokens)] (auto tokens) {
        for (auto& x : rm_tokens) {
            tokens.erase(x);
        }
        for (auto& x : add_tokens) {
            tokens.insert(x);
        }
        return update_tokens(tokens).then([tokens] {
            return tokens;
        });
    });
}

future<std::unordered_map<gms::inet_address, std::unordered_set<dht::token>>> load_tokens() {
    sstring req = format("SELECT peer, tokens FROM system.{}", PEERS);
    return execute_cql(req).then([] (::shared_ptr<cql3::untyped_result_set> cql_result) {
        std::unordered_map<gms::inet_address, std::unordered_set<dht::token>> ret;
        for (auto& row : *cql_result) {
            auto peer = gms::inet_address(row.get_as<net::inet_address>("peer"));
            if (row.has("tokens")) {
                auto blob = row.get_blob("tokens");
                auto cdef = peers()->get_column_definition("tokens");
                auto deserialized = cdef->type->deserialize(blob);
                auto tokens = value_cast<set_type_impl::native_type>(deserialized);
                ret.emplace(peer, decode_tokens(tokens));
            }
        }
        return ret;
    });
}

future<std::unordered_map<gms::inet_address, utils::UUID>> load_host_ids() {
    sstring req = format("SELECT peer, host_id FROM system.{}", PEERS);
    return execute_cql(req).then([] (::shared_ptr<cql3::untyped_result_set> cql_result) {
        std::unordered_map<gms::inet_address, utils::UUID> ret;
        for (auto& row : *cql_result) {
            auto peer = gms::inet_address(row.get_as<net::inet_address>("peer"));
            if (row.has("host_id")) {
                ret.emplace(peer, row.get_as<utils::UUID>("host_id"));
            }
        }
        return ret;
    });
}

future<std::unordered_map<gms::inet_address, sstring>> load_peer_features() {
    sstring req = format("SELECT peer, supported_features FROM system.{}", PEERS);
    return execute_cql(req).then([] (::shared_ptr<cql3::untyped_result_set> cql_result) {
        std::unordered_map<gms::inet_address, sstring> ret;
        for (auto& row : *cql_result) {
            if (row.has("supported_features")) {
                ret.emplace(row.get_as<net::inet_address>("peer"),
                        row.get_as<sstring>("supported_features"));
            }
        }
        return ret;
    });
}

future<> update_preferred_ip(gms::inet_address ep, gms::inet_address preferred_ip) {
    sstring req = format("INSERT INTO system.{} (peer, preferred_ip) VALUES (?, ?)", PEERS);
    return execute_cql(req, ep.addr(), preferred_ip.addr()).discard_result().then([] {
        return force_blocking_flush(PEERS);
    });
}

future<std::unordered_map<gms::inet_address, gms::inet_address>> get_preferred_ips() {
    sstring req = format("SELECT peer, preferred_ip FROM system.{}", PEERS);
    return execute_cql(req).then([] (::shared_ptr<cql3::untyped_result_set> cql_res_set) {
        std::unordered_map<gms::inet_address, gms::inet_address> res;

        for (auto& r : *cql_res_set) {
            if (r.has("preferred_ip")) {
                res.emplace(gms::inet_address(r.get_as<net::inet_address>("peer")),
                            gms::inet_address(r.get_as<net::inet_address>("preferred_ip")));
            }
        }

        return res;
    });
}

template <typename Value>
static future<> update_cached_values(gms::inet_address ep, sstring column_name, Value value) {
    return make_ready_future<>();
}

template <>
future<> update_cached_values(gms::inet_address ep, sstring column_name, sstring value) {
    return _local_cache.invoke_on_all([ep = std::move(ep),
                                       column_name = std::move(column_name),
                                       value = std::move(value)] (local_cache& lc) {
        if (column_name == "data_center") {
            lc._cached_dc_rack_info[ep].dc = value;
        } else if (column_name == "rack") {
            lc._cached_dc_rack_info[ep].rack = value;
        }
        return make_ready_future<>();
    });
}

template <typename Value>
future<> update_peer_info(gms::inet_address ep, sstring column_name, Value value) {
    if (ep == utils::fb_utilities::get_broadcast_address()) {
        return make_ready_future<>();
    }

    return update_cached_values(ep, column_name, value).then([ep, column_name, value] {
        sstring req = format("INSERT INTO system.{} (peer, {}) VALUES (?, ?)", PEERS, column_name);
        return execute_cql(req, ep.addr(), value).discard_result();
    });
}
// sets are not needed, since tokens are updated by another method
template future<> update_peer_info<sstring>(gms::inet_address ep, sstring column_name, sstring);
template future<> update_peer_info<utils::UUID>(gms::inet_address ep, sstring column_name, utils::UUID);
template future<> update_peer_info<net::inet_address>(gms::inet_address ep, sstring column_name, net::inet_address);

future<> update_hints_dropped(gms::inet_address ep, utils::UUID time_period, int value) {
    // with 30 day TTL
    sstring req = format("UPDATE system.{} USING TTL 2592000 SET hints_dropped[ ? ] = ? WHERE peer = ?", PEER_EVENTS);
    return execute_cql(req, time_period, value, ep.addr()).discard_result();
}

future<> set_scylla_local_param(const sstring& key, const sstring& value) {
    sstring req = format("UPDATE system.{} SET value = ? WHERE key = ?", SCYLLA_LOCAL);
    return execute_cql(req, value, key).discard_result();
}

future<std::optional<sstring>> get_scylla_local_param(const sstring& key){
    sstring req = format("SELECT value FROM system.{} WHERE key = ?", SCYLLA_LOCAL);
    return execute_cql(req, key).then([] (::shared_ptr<cql3::untyped_result_set> res) {
        if (res->empty() || !res->one().has("value")) {
            return std::optional<sstring>();
        }
        return std::optional<sstring>(res->one().get_as<sstring>("value"));
    });
}

future<> update_schema_version(utils::UUID version) {
    sstring req = format("INSERT INTO system.{} (key, schema_version) VALUES (?, ?)", LOCAL);
    return execute_cql(req, sstring(LOCAL), version).discard_result();
}

/**
 * Remove stored tokens being used by another node
 */
future<> remove_endpoint(gms::inet_address ep) {
    return _local_cache.invoke_on_all([ep] (local_cache& lc) {
        lc._cached_dc_rack_info.erase(ep);
    }).then([ep] {
        sstring req = format("DELETE FROM system.{} WHERE peer = ?", PEERS);
        return execute_cql(req, ep.addr()).discard_result();
    }).then([] {
        return force_blocking_flush(PEERS);
    });
}

    /**
     * This method is used to update the System Keyspace with the new tokens for this node
    */
future<> update_tokens(std::unordered_set<dht::token> tokens) {
    if (tokens.empty()) {
        throw std::invalid_argument("remove_endpoint should be used instead");
    }

    sstring req = format("INSERT INTO system.{} (key, tokens) VALUES (?, ?)", LOCAL);
    auto set_type = set_type_impl::get_instance(utf8_type, true);
    return execute_cql(req, sstring(LOCAL), make_set_value(set_type, prepare_tokens(tokens))).discard_result().then([] {
        return force_blocking_flush(LOCAL);
    });
}

future<> force_blocking_flush(sstring cfname) {
    assert(qctx);
    return qctx->_db.invoke_on_all([cfname = std::move(cfname)](database& db) {
        // if (!Boolean.getBoolean("cassandra.unsafesystem"))
        column_family& cf = db.find_column_family(NAME, cfname);
        return cf.flush();
    });
}

/**
 * One of three things will happen if you try to read the system keyspace:
 * 1. files are present and you can read them: great
 * 2. no files are there: great (new node is assumed)
 * 3. files are present but you can't read them: bad
 */
future<> check_health() {
    using namespace cql_transport::messages;
    sstring req = format("SELECT cluster_name FROM system.{} WHERE key=?", LOCAL);
    return execute_cql(req, sstring(LOCAL)).then([] (::shared_ptr<cql3::untyped_result_set> msg) {
        if (msg->empty() || !msg->one().has("cluster_name")) {
            // this is a brand new node
            sstring ins_req = format("INSERT INTO system.{} (key, cluster_name) VALUES (?, ?)", LOCAL);
            return execute_cql(ins_req, sstring(LOCAL), qctx->db().get_config().cluster_name()).discard_result();
        } else {
            auto saved_cluster_name = msg->one().get_as<sstring>("cluster_name");
            auto cluster_name = qctx->db().get_config().cluster_name();

            if (cluster_name != saved_cluster_name) {
                throw exceptions::configuration_exception("Saved cluster name " + saved_cluster_name + " != configured name " + cluster_name);
            }

            return make_ready_future<>();
        }
    });
}

future<std::unordered_set<dht::token>> get_saved_tokens() {
    sstring req = format("SELECT tokens FROM system.{} WHERE key = ?", LOCAL);
    return execute_cql(req, sstring(LOCAL)).then([] (auto msg) {
        if (msg->empty() || !msg->one().has("tokens")) {
            return make_ready_future<std::unordered_set<dht::token>>();
        }

        auto blob = msg->one().get_blob("tokens");
        auto cdef = local()->get_column_definition("tokens");
        auto deserialized = cdef->type->deserialize(blob);
        auto tokens = value_cast<set_type_impl::native_type>(deserialized);

        return make_ready_future<std::unordered_set<dht::token>>(decode_tokens(tokens));
    });
}

bool bootstrap_complete() {
    return get_bootstrap_state() == bootstrap_state::COMPLETED;
}

bool bootstrap_in_progress() {
    return get_bootstrap_state() == bootstrap_state::IN_PROGRESS;
}

bool was_decommissioned() {
    return get_bootstrap_state() == bootstrap_state::DECOMMISSIONED;
}

bootstrap_state get_bootstrap_state() {
    return _local_cache.local()._state;
}

future<> set_bootstrap_state(bootstrap_state state) {
    static std::unordered_map<bootstrap_state, sstring, enum_hash<bootstrap_state>> state_to_name({
        { bootstrap_state::NEEDS_BOOTSTRAP, "NEEDS_BOOTSTRAP" },
        { bootstrap_state::COMPLETED, "COMPLETED" },
        { bootstrap_state::IN_PROGRESS, "IN_PROGRESS" },
        { bootstrap_state::DECOMMISSIONED, "DECOMMISSIONED" }
    });

    sstring state_name = state_to_name.at(state);

    sstring req = format("INSERT INTO system.{} (key, bootstrapped) VALUES (?, ?)", LOCAL);
    return execute_cql(req, sstring(LOCAL), state_name).discard_result().then([state] {
        return force_blocking_flush(LOCAL).then([state] {
            return _local_cache.invoke_on_all([state] (local_cache& lc) {
                lc._state = state;
            });
        });
    });
}

std::vector<schema_ptr> all_tables() {
    std::vector<schema_ptr> r;
    auto schema_tables = db::schema_tables::all_tables(schema_features::full());
    std::copy(schema_tables.begin(), schema_tables.end(), std::back_inserter(r));
    r.insert(r.end(), { built_indexes(), hints(), batchlog(), paxos(), local(),
                    peers(), peer_events(), range_xfers(),
                    compactions_in_progress(), compaction_history(),
                    sstable_activity(), size_estimates(), large_partitions(), large_rows(), large_cells(),
                    scylla_local(), v3::views_builds_in_progress(), v3::built_views(),
                    v3::scylla_views_builds_in_progress(),
                    v3::truncated(),
    });
    // legacy schema
    r.insert(r.end(), {
                    // TODO: once we migrate hints/batchlog and add convertor
                    // legacy::hints(), legacy::batchlog(),
                    legacy::keyspaces(), legacy::column_families(),
                    legacy::columns(), legacy::triggers(), legacy::usertypes(),
                    legacy::functions(), legacy::aggregates(), });

    return r;
}

static void maybe_add_virtual_reader(schema_ptr s, database& db) {
    if (s.get() == size_estimates().get()) {
        db.find_column_family(s).set_virtual_reader(mutation_source(db::size_estimates::virtual_reader()));
    }
    if (s.get() == v3::views_builds_in_progress().get()) {
        db.find_column_family(s).set_virtual_reader(mutation_source(db::view::build_progress_virtual_reader(db)));
    }
    if (s.get() == built_indexes().get()) {
        db.find_column_family(s).set_virtual_reader(mutation_source(db::index::built_indexes_virtual_reader(db)));
    }
}

static bool maybe_write_in_user_memory(schema_ptr s, database& db) {
    return (s.get() == batchlog().get())
            || s == v3::scylla_views_builds_in_progress();
}

void make(database& db, bool durable, bool volatile_testing_only) {
    for (auto&& table : all_tables()) {
        auto ks_name = table->ks_name();
        if (!db.has_keyspace(ks_name)) {
            auto ksm = make_lw_shared<keyspace_metadata>(ks_name,
                    "org.apache.cassandra.locator.LocalStrategy",
                    std::map<sstring, sstring>{},
                    durable
                    );
            auto kscfg = db.make_keyspace_config(*ksm);
            kscfg.enable_disk_reads = !volatile_testing_only;
            kscfg.enable_disk_writes = !volatile_testing_only;
            kscfg.enable_commitlog = !volatile_testing_only;
            kscfg.enable_cache = true;
            // don't make system keyspace reads wait for user reads
            kscfg.read_concurrency_semaphore = &db._system_read_concurrency_sem;
            // don't make system keyspace writes wait for user writes (if under pressure)
            kscfg.dirty_memory_manager = &db._system_dirty_memory_manager;
            keyspace _ks{ksm, std::move(kscfg)};
            auto rs(locator::abstract_replication_strategy::create_replication_strategy(NAME, "LocalStrategy", service::get_local_storage_service().get_token_metadata(), ksm->strategy_options()));
            _ks.set_replication_strategy(std::move(rs));
            db.add_keyspace(ks_name, std::move(_ks));
        }
        auto& ks = db.find_keyspace(ks_name);
        auto cfg = ks.make_column_family_config(*table, db);
        if (maybe_write_in_user_memory(table, db)) {
            cfg.dirty_memory_manager = &db._dirty_memory_manager;
        } else {
            cfg.memtable_scheduling_group = default_scheduling_group();
            cfg.memtable_to_cache_scheduling_group = default_scheduling_group();
        }
        db.add_column_family(ks, table, std::move(cfg));
        maybe_add_virtual_reader(table, db);
    }
}

future<utils::UUID> get_local_host_id() {
    using namespace cql_transport::messages;
    sstring req = format("SELECT host_id FROM system.{} WHERE key=?", LOCAL);
    return execute_cql(req, sstring(LOCAL)).then([] (::shared_ptr<cql3::untyped_result_set> msg) {
        auto new_id = [] {
            auto host_id = utils::make_random_uuid();
            return set_local_host_id(host_id);
        };
        if (msg->empty() || !msg->one().has("host_id")) {
            return new_id();
        }

        auto host_id = msg->one().get_as<utils::UUID>("host_id");
        return make_ready_future<utils::UUID>(host_id);
    });
}

future<utils::UUID> set_local_host_id(const utils::UUID& host_id) {
    sstring req = format("INSERT INTO system.{} (key, host_id) VALUES (?, ?)", LOCAL);
    return execute_cql(req, sstring(LOCAL), host_id).then([] (auto msg) {
        return force_blocking_flush(LOCAL);
    }).then([host_id] {
        return host_id;
    });
}

std::unordered_map<gms::inet_address, locator::endpoint_dc_rack>
load_dc_rack_info() {
    return _local_cache.local()._cached_dc_rack_info;
}


future<foreign_ptr<lw_shared_ptr<reconcilable_result>>>
query_mutations(distributed<service::storage_proxy>& proxy, const sstring& cf_name) {
    return query_mutations(proxy, db::system_keyspace::NAME, cf_name);
}

future<lw_shared_ptr<query::result_set>>
query(distributed<service::storage_proxy>& proxy, const sstring& cf_name) {
    return query(proxy, db::system_keyspace::NAME, cf_name);
}

future<lw_shared_ptr<query::result_set>>
query(distributed<service::storage_proxy>& proxy, const sstring& cf_name, const dht::decorated_key& key, query::clustering_range row_range)
{
    return query(proxy, db::system_keyspace::NAME, cf_name, key, row_range);
}

future<foreign_ptr<lw_shared_ptr<reconcilable_result>>>
query_mutations(distributed<service::storage_proxy>& proxy, const sstring& ks_name, const sstring& cf_name) {
    database& db = proxy.local().get_db().local();
    schema_ptr schema = db.find_schema(ks_name, cf_name);
    auto slice = partition_slice_builder(*schema).build();
    auto cmd = make_lw_shared<query::read_command>(schema->id(), schema->version(),
        std::move(slice), std::numeric_limits<uint32_t>::max());
    return proxy.local().query_mutations_locally(std::move(schema), std::move(cmd), query::full_partition_range, db::no_timeout)
            .then([] (foreign_ptr<lw_shared_ptr<reconcilable_result>> rr, auto ht) { return std::move(rr); });
}

future<lw_shared_ptr<query::result_set>>
query(distributed<service::storage_proxy>& proxy, const sstring& ks_name, const sstring& cf_name) {
    database& db = proxy.local().get_db().local();
    schema_ptr schema = db.find_schema(ks_name, cf_name);
    auto slice = partition_slice_builder(*schema).build();
    auto cmd = make_lw_shared<query::read_command>(schema->id(), schema->version(),
        std::move(slice), std::numeric_limits<uint32_t>::max());
    return proxy.local().query(schema, cmd, {query::full_partition_range}, db::consistency_level::ONE,
            {db::no_timeout, nullptr}).then([schema, cmd] (auto&& qr) {
        return make_lw_shared(query::result_set::from_raw_result(schema, cmd->slice, *qr.query_result));
    });
}

future<lw_shared_ptr<query::result_set>>
query(distributed<service::storage_proxy>& proxy, const sstring& ks_name, const sstring& cf_name, const dht::decorated_key& key, query::clustering_range row_range)
{
    auto&& db = proxy.local().get_db().local();
    auto schema = db.find_schema(ks_name, cf_name);
    auto slice = partition_slice_builder(*schema)
        .with_range(std::move(row_range))
        .build();
    auto cmd = make_lw_shared<query::read_command>(schema->id(), schema->version(), std::move(slice), query::max_rows);

    return proxy.local().query(schema, cmd, {dht::partition_range::make_singular(key)}, db::consistency_level::ONE,
            {db::no_timeout, nullptr}).then([schema, cmd] (auto&& qr) {
        return make_lw_shared(query::result_set::from_raw_result(schema, cmd->slice, *qr.query_result));
    });
}

static map_type_impl::native_type prepare_rows_merged(std::unordered_map<int32_t, int64_t>& rows_merged) {
    map_type_impl::native_type tmp;
    for (auto& r: rows_merged) {
        int32_t first = r.first;
        int64_t second = r.second;
        auto map_element = std::make_pair<data_value, data_value>(data_value(first), data_value(second));
        tmp.push_back(std::move(map_element));
    }
    return tmp;
}

future<> update_compaction_history(sstring ksname, sstring cfname, int64_t compacted_at, int64_t bytes_in, int64_t bytes_out,
                                   std::unordered_map<int32_t, int64_t> rows_merged)
{
    // don't write anything when the history table itself is compacted, since that would in turn cause new compactions
    if (ksname == "system" && cfname == COMPACTION_HISTORY) {
        return make_ready_future<>();
    }

    auto map_type = map_type_impl::get_instance(int32_type, long_type, true);

    sstring req = format("INSERT INTO system.{} (id, keyspace_name, columnfamily_name, compacted_at, bytes_in, bytes_out, rows_merged) VALUES (?, ?, ?, ?, ?, ?, ?)"
                    , COMPACTION_HISTORY);

    return execute_cql(req, utils::UUID_gen::get_time_UUID(), ksname, cfname, compacted_at, bytes_in, bytes_out,
                       make_map_value(map_type, prepare_rows_merged(rows_merged))).discard_result().handle_exception([] (auto ep) {
        slogger.error("update compaction history failed: {}: ignored", ep);
    });
}

future<> get_compaction_history(compaction_history_consumer&& f) {
    return do_with(compaction_history_consumer(std::move(f)),
            [](compaction_history_consumer& consumer) mutable {
        sstring req = format("SELECT * from system.{}", COMPACTION_HISTORY);
        return qctx->qp().query(req, [&consumer] (const cql3::untyped_result_set::row& row) mutable {
            compaction_history_entry entry;
            entry.id = row.get_as<utils::UUID>("id");
            entry.ks = row.get_as<sstring>("keyspace_name");
            entry.cf = row.get_as<sstring>("columnfamily_name");
            entry.compacted_at = row.get_as<int64_t>("compacted_at");
            entry.bytes_in = row.get_as<int64_t>("bytes_in");
            entry.bytes_out = row.get_as<int64_t>("bytes_out");
            if (row.has("rows_merged")) {
                entry.rows_merged = row.get_map<int32_t, int64_t>("rows_merged");
            }
            return consumer(std::move(entry)).then([] {
                return stop_iteration::no;
            });
        });
    });
}

future<int> increment_and_get_generation() {
    auto req = format("SELECT gossip_generation FROM system.{} WHERE key='{}'", LOCAL, LOCAL);
    return qctx->qp().execute_internal(req).then([] (auto rs) {
        int generation;
        if (rs->empty() || !rs->one().has("gossip_generation")) {
            // seconds-since-epoch isn't a foolproof new generation
            // (where foolproof is "guaranteed to be larger than the last one seen at this ip address"),
            // but it's as close as sanely possible
            generation = service::get_generation_number();
        } else {
            // Other nodes will ignore gossip messages about a node that have a lower generation than previously seen.
            int stored_generation = rs->one().template get_as<int>("gossip_generation") + 1;
            int now = service::get_generation_number();
            if (stored_generation >= now) {
                slogger.warn("Using stored Gossip Generation {} as it is greater than current system time {}."
                            "See CASSANDRA-3654 if you experience problems", stored_generation, now);
                generation = stored_generation;
            } else {
                generation = now;
            }
        }
        auto req = format("INSERT INTO system.{} (key, gossip_generation) VALUES ('{}', ?)", LOCAL, LOCAL);
        return qctx->qp().execute_internal(req, {generation}).then([generation] (auto rs) {
            return force_blocking_flush(LOCAL);
        }).then([generation] {
            return make_ready_future<int>(generation);
        });
    });
}

mutation make_size_estimates_mutation(const sstring& ks, std::vector<range_estimates> estimates) {
    auto&& schema = db::system_keyspace::size_estimates();
    auto timestamp = api::new_timestamp();
    mutation m_to_apply{schema, partition_key::from_single_value(*schema, utf8_type->decompose(ks))};

    for (auto&& e : estimates) {
        auto ck = clustering_key_prefix(std::vector<bytes>{
                utf8_type->decompose(e.schema->cf_name()), e.range_start_token, e.range_end_token});

        m_to_apply.set_clustered_cell(ck, "mean_partition_size", e.mean_partition_size, timestamp);
        m_to_apply.set_clustered_cell(ck, "partitions_count", e.partitions_count, timestamp);
    }

    return m_to_apply;
}

future<> register_view_for_building(sstring ks_name, sstring view_name, const dht::token& token) {
    sstring req = format("INSERT INTO system.{} (keyspace_name, view_name, generation_number, cpu_id, first_token) VALUES (?, ?, ?, ?, ?)",
            v3::SCYLLA_VIEWS_BUILDS_IN_PROGRESS);
    return execute_cql(
            std::move(req),
            std::move(ks_name),
            std::move(view_name),
            0,
            int32_t(engine().cpu_id()),
            dht::global_partitioner().to_sstring(token)).discard_result();
}

future<> update_view_build_progress(sstring ks_name, sstring view_name, const dht::token& token) {
    sstring req = format("INSERT INTO system.{} (keyspace_name, view_name, next_token, cpu_id) VALUES (?, ?, ?, ?)",
            v3::SCYLLA_VIEWS_BUILDS_IN_PROGRESS);
    return execute_cql(
            std::move(req),
            std::move(ks_name),
            std::move(view_name),
            dht::global_partitioner().to_sstring(token),
            int32_t(engine().cpu_id())).discard_result();
}

future<> remove_view_build_progress_across_all_shards(sstring ks_name, sstring view_name) {
    return execute_cql(
            format("DELETE FROM system.{} WHERE keyspace_name = ? AND view_name = ?", v3::SCYLLA_VIEWS_BUILDS_IN_PROGRESS),
            std::move(ks_name),
            std::move(view_name)).discard_result();
}

future<> remove_view_build_progress(sstring ks_name, sstring view_name) {
    return execute_cql(
            format("DELETE FROM system.{} WHERE keyspace_name = ? AND view_name = ? AND cpu_id = ?", v3::SCYLLA_VIEWS_BUILDS_IN_PROGRESS),
            std::move(ks_name),
            std::move(view_name),
            int32_t(engine().cpu_id())).discard_result();
}

future<> mark_view_as_built(sstring ks_name, sstring view_name) {
    return execute_cql(
            format("INSERT INTO system.{} (keyspace_name, view_name) VALUES (?, ?)", v3::BUILT_VIEWS),
            std::move(ks_name),
            std::move(view_name)).discard_result();
}

future<> remove_built_view(sstring ks_name, sstring view_name) {
    return execute_cql(
            format("DELETE FROM system.{} WHERE keyspace_name = ? AND view_name = ?", v3::BUILT_VIEWS),
            std::move(ks_name),
            std::move(view_name)).discard_result();
}

future<std::vector<view_name>> load_built_views() {
    return execute_cql(format("SELECT * FROM system.{}", v3::BUILT_VIEWS)).then([] (::shared_ptr<cql3::untyped_result_set> cql_result) {
        return boost::copy_range<std::vector<view_name>>(*cql_result
                | boost::adaptors::transformed([] (const cql3::untyped_result_set::row& row) {
            auto ks_name = row.get_as<sstring>("keyspace_name");
            auto cf_name = row.get_as<sstring>("view_name");
            return std::pair(std::move(ks_name), std::move(cf_name));
        }));
    });
}

future<std::vector<view_build_progress>> load_view_build_progress() {
    return execute_cql(format("SELECT keyspace_name, view_name, first_token, next_token, cpu_id FROM system.{}",
            v3::SCYLLA_VIEWS_BUILDS_IN_PROGRESS)).then([] (::shared_ptr<cql3::untyped_result_set> cql_result) {
        std::vector<view_build_progress> progress;
        for (auto& row : *cql_result) {
            auto ks_name = row.get_as<sstring>("keyspace_name");
            auto cf_name = row.get_as<sstring>("view_name");
            auto first_token = dht::global_partitioner().from_sstring(row.get_as<sstring>("first_token"));
            auto next_token_sstring = row.get_opt<sstring>("next_token");
            std::optional<dht::token> next_token;
            if (next_token_sstring) {
                next_token = dht::global_partitioner().from_sstring(std::move(next_token_sstring).value());
            }
            auto cpu_id = row.get_as<int32_t>("cpu_id");
            progress.emplace_back(view_build_progress{
                    view_name(std::move(ks_name), std::move(cf_name)),
                    std::move(first_token),
                    std::move(next_token),
                    static_cast<shard_id>(cpu_id)});
        }
        return progress;
    });
}

} // namespace system_keyspace

sstring system_keyspace_name() {
    return system_keyspace::NAME;
}

} // namespace db
