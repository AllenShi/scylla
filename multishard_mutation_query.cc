/*
 * Copyright (C) 2018 ScyllaDB
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

#include "schema_registry.hh"
#include "service/priority_manager.hh"
#include "multishard_mutation_query.hh"

#include <boost/range/adaptor/reversed.hpp>

logging::logger mmq_log("multishard_mutation_query");

template <typename T>
using foreign_unique_ptr = foreign_ptr<std::unique_ptr<T>>;

class read_context {
    struct reader_params {
        std::unique_ptr<const dht::partition_range> range;
        std::unique_ptr<const query::partition_slice> slice;

        reader_params(dht::partition_range range, query::partition_slice slice)
            : range(std::make_unique<const dht::partition_range>(std::move(range)))
            , slice(std::make_unique<const query::partition_slice>(std::move(slice))) {
        }
        reader_params(std::unique_ptr<const dht::partition_range> range, std::unique_ptr<const query::partition_slice> slice)
            : range(std::move(range))
            , slice(std::move(slice)) {
        }
    };

    struct bundled_remote_reader {
        foreign_unique_ptr<reader_params> params;
        foreign_unique_ptr<utils::phased_barrier::operation> read_operation;
        foreign_unique_ptr<flat_mutation_reader> reader;
    };

    using inexistent_state = std::monostate;
    struct successful_lookup_state {
        foreign_unique_ptr<reader_params> params;
        foreign_unique_ptr<utils::phased_barrier::operation> read_operation;
        foreign_unique_ptr<flat_mutation_reader> reader;
    };
    struct used_state {
        foreign_unique_ptr<reader_params> params;
        foreign_unique_ptr<utils::phased_barrier::operation> read_operation;
    };
    struct dismantling_state {
        foreign_unique_ptr<reader_params> params;
        foreign_unique_ptr<utils::phased_barrier::operation> read_operation;
        future<stopped_foreign_reader> reader_fut;
        circular_buffer<mutation_fragment> buffer;
    };
    struct ready_to_save_state {
        foreign_unique_ptr<reader_params> params;
        foreign_unique_ptr<utils::phased_barrier::operation> read_operation;
        foreign_unique_ptr<flat_mutation_reader> reader;
        circular_buffer<mutation_fragment> buffer;
    };
    struct future_used_state {
        future<used_state> fut;
    };
    struct future_dismantling_state {
        future<dismantling_state> fut;
    };

    //                           ( )
    //                            |
    //            +------ inexistent_state -----+
    //            |                             |
    //        (1) |                         (6) |
    //            |                             |
    //  successful_lookup_state         future_used_state
    //     |              |               |           |
    // (2) |          (3) |           (7) |       (8) |
    //     |              |               |           |
    //     |         used_state <---------+  future_dismantling_state
    //     |              |                           |
    //     |          (4) |                       (9) |
    //     |              |                           |
    //     |      dismantling_state <-----------------+
    //     |              |
    //     |          (5) |
    //     |              |
    //     +----> ready_to_save_state
    //                    |
    //                   (O)
    //
    //  1) lookup_readers()
    //  2) save_readers()
    //  3) make_remote_reader()
    //  4) dismantle_reader()
    //  5) prepare_reader_for_saving()
    //  6) do_make_remote_reader()
    //  7) reader is created
    //  8) dismantle_reader()
    //  9) reader is created
    using reader_state = std::variant<
        inexistent_state,
        successful_lookup_state,
        used_state,
        dismantling_state,
        ready_to_save_state,
        future_used_state,
        future_dismantling_state>;

    distributed<database>& _db;
    schema_ptr _schema;
    const query::read_command& _cmd;
    const dht::partition_range_vector& _ranges;
    tracing::trace_state_ptr _trace_state;

    // One for each shard. Index is shard id.
    std::vector<reader_state> _readers;

    static future<bundled_remote_reader> do_make_remote_reader(
            distributed<database>& db,
            shard_id shard,
            schema_ptr schema,
            const dht::partition_range& pr,
            const query::partition_slice& ps,
            const io_priority_class& pc,
            tracing::trace_state_ptr trace_state);

    future<foreign_unique_ptr<flat_mutation_reader>> make_remote_reader(
            shard_id shard,
            schema_ptr schema,
            const dht::partition_range& pr,
            const query::partition_slice& ps,
            const io_priority_class& pc,
            tracing::trace_state_ptr trace_state,
            streamed_mutation::forwarding fwd_sm,
            mutation_reader::forwarding fwd_mr);

    void dismantle_reader(shard_id shard, future<stopped_foreign_reader>&& stopped_reader_fut);

    future<> cleanup_readers();

    ready_to_save_state* prepare_reader_for_saving(dismantling_state& current_state, future<stopped_foreign_reader>&& stopped_reader_fut,
            const dht::decorated_key& last_pkey, const std::optional<clustering_key_prefix>& last_ckey);
    void dismantle_combined_buffer(circular_buffer<mutation_fragment> combined_buffer, const dht::decorated_key& pkey);
    void dismantle_compaction_state(detached_compaction_state compaction_state);
    future<> save_reader(ready_to_save_state& current_state, const dht::decorated_key& last_pkey,
            const std::optional<clustering_key_prefix>& last_ckey);

public:
    read_context(distributed<database>& db, schema_ptr s, const query::read_command& cmd, const dht::partition_range_vector& ranges,
            tracing::trace_state_ptr trace_state)
            : _db(db)
            , _schema(std::move(s))
            , _cmd(cmd)
            , _ranges(ranges)
            , _trace_state(std::move(trace_state)) {
        _readers.resize(smp::count);
    }

    read_context(read_context&&) = delete;
    read_context(const read_context&) = delete;

    read_context& operator=(read_context&&) = delete;
    read_context& operator=(const read_context&) = delete;

    remote_reader_factory factory() {
        return [this] (
                shard_id shard,
                schema_ptr schema,
                const dht::partition_range& pr,
                const query::partition_slice& ps,
                const io_priority_class& pc,
                tracing::trace_state_ptr trace_state,
                streamed_mutation::forwarding fwd_sm,
                mutation_reader::forwarding fwd_mr) {
            return make_remote_reader(shard, std::move(schema), pr, ps, pc, std::move(trace_state), fwd_sm, fwd_mr);
        };
    }

    foreign_reader_dismantler dismantler() {
        return [this] (shard_id shard, future<stopped_foreign_reader>&& stopped_reader_fut) {
            dismantle_reader(shard, std::move(stopped_reader_fut));
        };
    }

    future<> lookup_readers();

    future<> save_readers(circular_buffer<mutation_fragment> unconsumed_buffer, detached_compaction_state compaction_state,
            std::optional<clustering_key_prefix> last_ckey);
};

future<read_context::bundled_remote_reader> read_context::do_make_remote_reader(
        distributed<database>& db,
        shard_id shard,
        schema_ptr schema,
        const dht::partition_range& pr,
        const query::partition_slice& ps,
        const io_priority_class&,
        tracing::trace_state_ptr trace_state) {
    return db.invoke_on(shard, [gs = global_schema_ptr(schema), &pr, &ps, gts = tracing::global_trace_state_ptr(std::move(trace_state))] (
                database& db) {
        auto s = gs.get();
        auto& table = db.find_column_family(s);
        //TODO need a way to transport io_priority_calls across shards
        auto& pc = service::get_local_sstable_query_read_priority();
        auto params = reader_params(pr, ps);
        auto read_operation = table.read_in_progress();
        auto reader = table.as_mutation_source().make_reader(std::move(s), *params.range, *params.slice, pc, gts.get());

        return make_ready_future<bundled_remote_reader>(bundled_remote_reader{
                make_foreign(std::make_unique<reader_params>(std::move(params))),
                make_foreign(std::make_unique<utils::phased_barrier::operation>(std::move(read_operation))),
                make_foreign(std::make_unique<flat_mutation_reader>(std::move(reader)))});
    });
}

future<foreign_unique_ptr<flat_mutation_reader>> read_context::make_remote_reader(
        shard_id shard,
        schema_ptr schema,
        const dht::partition_range& pr,
        const query::partition_slice& ps,
        const io_priority_class& pc,
        tracing::trace_state_ptr trace_state,
        streamed_mutation::forwarding,
        mutation_reader::forwarding) {
    auto& rs = _readers[shard];

    if (!std::holds_alternative<successful_lookup_state>(rs) && !std::holds_alternative<inexistent_state>(rs)) {
        mmq_log.warn("Unexpected request to create reader for shard {}. A reader for this shard was already created.", shard);
        throw std::logic_error(sprint("Unexpected request to create reader for shard {}."
                    " A reader for this shard was already created in the context of this read.", shard));
    }

    // The reader is either in inexistent or successful lookup state.
    if (auto current_state = std::get_if<successful_lookup_state>(&rs)) {
        auto reader = std::move(current_state->reader);
        rs = used_state{std::move(current_state->params), std::move(current_state->read_operation)};
        return make_ready_future<foreign_unique_ptr<flat_mutation_reader>>(std::move(reader));
    }

    auto created = promise<used_state>();
    rs = future_used_state{created.get_future()};
    return do_make_remote_reader(_db, shard, std::move(schema), pr, ps, pc, std::move(trace_state)).then([this, &rs, created = std::move(created)] (
                bundled_remote_reader&& bundled_reader) mutable {
        auto new_state = used_state{std::move(bundled_reader.params), std::move(bundled_reader.read_operation)};
        if (std::holds_alternative<future_used_state>(rs)) {
            rs = std::move(new_state);
        } else {
            created.set_value(std::move(new_state));
        }
        return std::move(bundled_reader.reader);
    });
}

void read_context::dismantle_reader(shard_id shard, future<stopped_foreign_reader>&& stopped_reader_fut) {
    auto& rs = _readers[shard];

    if (auto* maybe_used_state = std::get_if<used_state>(&rs)) {
        auto read_operation = std::move(maybe_used_state->read_operation);
        auto params = std::move(maybe_used_state->params);
        rs = dismantling_state{std::move(params), std::move(read_operation), std::move(stopped_reader_fut), circular_buffer<mutation_fragment>{}};
    } else if (auto* maybe_future_used_state = std::get_if<future_used_state>(&rs)) {
        auto f = maybe_future_used_state->fut.then([stopped_reader_fut = std::move(stopped_reader_fut)] (used_state&& current_state) mutable {
            auto read_operation = std::move(current_state.read_operation);
            auto params = std::move(current_state.params);
            return dismantling_state{std::move(params), std::move(read_operation), std::move(stopped_reader_fut),
                circular_buffer<mutation_fragment>{}};
        });
        rs = future_dismantling_state{std::move(f)};
    } else {
        mmq_log.warn("Unexpected request to dismantle reader for shard {}. Reader was not created nor is in the process of being created.", shard);
    }
}

future<> read_context::cleanup_readers() {
    auto cleanup = [db = &_db.local()] (shard_id shard, dismantling_state state) {
        return state.reader_fut.then_wrapped([db, shard, params = std::move(state.params),
                read_operation = std::move(state.read_operation)] (future<stopped_foreign_reader>&& fut) mutable {
            if (fut.failed()) {
                mmq_log.debug("Failed to stop reader on shard {}: {}", shard, fut.get_exception());
                ++db->get_stats().multishard_query_failed_reader_stops;
            } else {
                smp::submit_to(shard, [reader = fut.get0().remote_reader, params = std::move(params),
                        read_operation = std::move(read_operation)] () mutable {
                    reader.release();
                    params.release();
                    read_operation.release();
                });
            }
        });
    };

    std::vector<future<>> futures;

    // Wait for pending read-aheads in the background.
    for (shard_id shard = 0; shard != smp::count; ++shard) {
        auto& rs = _readers[shard];

        if (auto maybe_dismantling_state = std::get_if<dismantling_state>(&rs)) {
            cleanup(shard, std::move(*maybe_dismantling_state));
        } else if (auto maybe_future_dismantling_state = std::get_if<future_dismantling_state>(&rs)) {
            futures.emplace_back(maybe_future_dismantling_state->fut.then([=] (dismantling_state&& current_state) {
                cleanup(shard, std::move(current_state));
            }));
        }
    }

    return when_all(futures.begin(), futures.end()).discard_result();
}

void read_context::dismantle_combined_buffer(circular_buffer<mutation_fragment> combined_buffer, const dht::decorated_key& pkey) {
    auto& partitioner = dht::global_partitioner();

    std::vector<mutation_fragment> tmp_buffer;

    auto rit = std::reverse_iterator(combined_buffer.end());
    const auto rend = std::reverse_iterator(combined_buffer.begin());
    for (;rit != rend; ++rit) {
        if (rit->is_partition_start()) {
            const auto shard = partitioner.shard_of(rit->as_partition_start().key().token());
            auto& shard_buffer = std::get<dismantling_state>(_readers[shard]).buffer;
            for (auto& smf : tmp_buffer) {
                shard_buffer.emplace_front(std::move(smf));
            }
            shard_buffer.emplace_front(std::move(*rit));
            tmp_buffer.clear();
        } else {
            tmp_buffer.emplace_back(std::move(*rit));
        }
    }

    const auto shard = partitioner.shard_of(pkey.token());
    auto& shard_buffer = std::get<dismantling_state>(_readers[shard]).buffer;
    for (auto& smf : tmp_buffer) {
        shard_buffer.emplace_front(std::move(smf));
    }
}

void read_context::dismantle_compaction_state(detached_compaction_state compaction_state) {
    auto& partitioner = dht::global_partitioner();
    const auto shard = partitioner.shard_of(compaction_state.partition_start.key().token());
    auto& shard_buffer = std::get<dismantling_state>(_readers[shard]).buffer;

    for (auto& rt : compaction_state.range_tombstones | boost::adaptors::reversed) {
        shard_buffer.emplace_front(std::move(rt));
    }

    if (compaction_state.static_row) {
        shard_buffer.emplace_front(std::move(*compaction_state.static_row));
    }

    shard_buffer.emplace_front(std::move(compaction_state.partition_start));
}

read_context::ready_to_save_state* read_context::prepare_reader_for_saving(
        dismantling_state& current_state,
        future<stopped_foreign_reader>&& stopped_reader_fut,
        const dht::decorated_key& last_pkey,
        const std::optional<clustering_key_prefix>& last_ckey) {
    const auto shard = current_state.params.get_owner_shard();
    auto& rs = _readers[shard];

    if (stopped_reader_fut.failed()) {
        mmq_log.debug("Failed to stop reader on shard {}: {}", shard, stopped_reader_fut.get_exception());
        ++_db.local().get_stats().multishard_query_failed_reader_stops;
        return nullptr;
    }

    auto stopped_reader = stopped_reader_fut.get0();

    // If the buffer is empty just overwrite it.
    // If it has some data in it append the fragments to the back.
    // The unconsumed fragments appended here come from the
    // foreign_reader which is at the lowest layer, hence its
    // fragments need to be at the back of the buffer.
    if (current_state.buffer.empty()) {
        current_state.buffer = std::move(stopped_reader.unconsumed_fragments);
    } else {
        std::move(stopped_reader.unconsumed_fragments.begin(), stopped_reader.unconsumed_fragments.end(), std::back_inserter(current_state.buffer));
    }
    rs = ready_to_save_state{std::move(current_state.params), std::move(current_state.read_operation), std::move(stopped_reader.remote_reader),
        std::move(current_state.buffer)};
    return &std::get<ready_to_save_state>(rs);
}

future<> read_context::save_reader(ready_to_save_state& current_state, const dht::decorated_key& last_pkey,
        const std::optional<clustering_key_prefix>& last_ckey) {
    const auto shard = current_state.reader.get_owner_shard();
    return _db.invoke_on(shard, [shard, query_uuid = _cmd.query_uuid, query_ranges = _ranges, &current_state, &last_pkey, &last_ckey,
            gts = tracing::global_trace_state_ptr(_trace_state)] (database& db) mutable {
        try {
            auto params = current_state.params.release();
            auto read_operation = current_state.read_operation.release();
            auto reader = current_state.reader.release();
            auto& buffer = current_state.buffer;
            const auto fragments = buffer.size();
            const auto size_before = reader->buffer_size();

            auto rit = std::reverse_iterator(buffer.cend());
            auto rend = std::reverse_iterator(buffer.cbegin());
            auto& schema = *reader->schema();
            for (;rit != rend; ++rit) {
                // Copy the fragment, the buffer is on another shard.
                reader->unpop_mutation_fragment(mutation_fragment(schema, *rit));
            }

            const auto size_after = reader->buffer_size();

            auto querier = query::shard_mutation_querier(
                    std::move(query_ranges),
                    std::move(params->range),
                    std::move(params->slice),
                    std::move(*reader),
                    last_pkey,
                    last_ckey);

            db.get_querier_cache().insert(query_uuid, std::move(querier), gts.get());

            db.get_stats().multishard_query_unpopped_fragments += fragments;
            db.get_stats().multishard_query_unpopped_bytes += (size_after - size_before);
        } catch (...) {
            // We don't want to fail a read just because of a failure to
            // save any of the readers.
            mmq_log.debug("Failed to save reader: {}", std::current_exception());
            ++db.get_stats().multishard_query_failed_reader_saves;
        }
    }).handle_exception([this, shard] (std::exception_ptr e) {
        // We don't want to fail a read just because of a failure to
        // save any of the readers.
        mmq_log.debug("Failed to save reader on shard {}: {}", shard, e);
        // This will account the failure on the local shard but we don't
        // know where exactly the failure happened anyway.
        ++_db.local().get_stats().multishard_query_failed_reader_saves;
    });
}

future<> read_context::lookup_readers() {
    if (_cmd.query_uuid == utils::UUID{} || _cmd.is_first_page) {
        return make_ready_future<>();
    }

    return parallel_for_each(boost::irange(0u, smp::count), [this] (shard_id shard) {
        return _db.invoke_on(shard,
                [shard, cmd = &_cmd, ranges = &_ranges, gs = global_schema_ptr(_schema), gts = tracing::global_trace_state_ptr(_trace_state)] (
                        database& db) mutable -> reader_state {
            auto schema = gs.get();
            auto querier_opt = db.get_querier_cache().lookup_shard_mutation_querier(cmd->query_uuid, *schema, *ranges, cmd->slice, gts.get());
            if (!querier_opt) {
                return inexistent_state{};
            }

            auto& q = *querier_opt;
            auto& table = db.find_column_family(schema);
            auto params = make_foreign(std::make_unique<reader_params>(std::move(q).reader_range(), std::move(q).reader_slice()));
            auto read_operation = make_foreign(std::make_unique<utils::phased_barrier::operation>(table.read_in_progress()));
            auto reader = make_foreign(std::make_unique<flat_mutation_reader>(std::move(q).reader()));
            return successful_lookup_state{std::move(params), std::move(read_operation), std::move(reader)};
        }).then([this, shard] (reader_state&& state) {
            _readers[shard] = std::move(state);
        });
    });
}

future<> read_context::save_readers(circular_buffer<mutation_fragment> unconsumed_buffer, detached_compaction_state compaction_state,
            std::optional<clustering_key_prefix> last_ckey) {
    if (_cmd.query_uuid == utils::UUID{}) {
        return cleanup_readers();
    }

    auto last_pkey = compaction_state.partition_start.key();
    dismantle_combined_buffer(std::move(unconsumed_buffer), last_pkey);
    dismantle_compaction_state(std::move(compaction_state));

    return do_with(std::move(last_pkey), std::move(last_ckey), [this] (const dht::decorated_key& last_pkey,
                const std::optional<clustering_key_prefix>& last_ckey) {
        return parallel_for_each(_readers, [this, &last_pkey, &last_ckey] (reader_state& rs) {
            if (auto* maybe_successful_lookup_state = std::get_if<successful_lookup_state>(&rs)) {
                auto& current_state = *maybe_successful_lookup_state;
                rs = ready_to_save_state{std::move(current_state.params), std::move(current_state.read_operation),
                        std::move(current_state.reader), circular_buffer<mutation_fragment>{}};
                return save_reader(std::get<ready_to_save_state>(rs), last_pkey, last_ckey);
            }

            auto finish_saving = [this, &last_pkey, &last_ckey] (dismantling_state& current_state) {
                return current_state.reader_fut.then_wrapped([this, &current_state, &last_pkey, &last_ckey] (
                            future<stopped_foreign_reader>&& stopped_reader_fut) mutable {
                    if (auto* ready_state = prepare_reader_for_saving(current_state, std::move(stopped_reader_fut), last_pkey, last_ckey)) {
                        return save_reader(*ready_state, last_pkey, last_ckey);
                    }
                    return make_ready_future<>();
                });
            };

            if (auto* maybe_dismantling_state = std::get_if<dismantling_state>(&rs)) {
                return finish_saving(*maybe_dismantling_state);
            }

            if (auto* maybe_future_dismantling_state = std::get_if<future_dismantling_state>(&rs)) {
                return maybe_future_dismantling_state->fut.then([this, &rs,
                        finish_saving = std::move(finish_saving)] (dismantling_state&& next_state) mutable {
                    rs = std::move(next_state);
                    return finish_saving(std::get<dismantling_state>(rs));
                });
            }

            return make_ready_future<>();
        });
    });
}

static future<reconcilable_result> do_query_mutations(
        distributed<database>& db,
        schema_ptr s,
        const query::read_command& cmd,
        const dht::partition_range_vector& ranges,
        tracing::trace_state_ptr trace_state,
        db::timeout_clock::time_point timeout,
        query::result_memory_accounter&& accounter) {
    return do_with(std::make_unique<read_context>(db, s, cmd, ranges, trace_state), [s, &cmd, &ranges, trace_state, timeout,
            accounter = std::move(accounter)] (std::unique_ptr<read_context>& ctx) mutable {
        return ctx->lookup_readers().then([&ctx, s = std::move(s), &cmd, &ranges, trace_state, timeout,
                accounter = std::move(accounter)] () mutable {
            auto ms = mutation_source([&] (schema_ptr s,
                    const dht::partition_range& pr,
                    const query::partition_slice& ps,
                    const io_priority_class& pc,
                    tracing::trace_state_ptr trace_state,
                    streamed_mutation::forwarding fwd_sm,
                    mutation_reader::forwarding fwd_mr) {
                return make_multishard_combining_reader(std::move(s), pr, ps, pc, dht::global_partitioner(), ctx->factory(), std::move(trace_state),
                        fwd_sm, fwd_mr, ctx->dismantler());
            });
            auto reader = make_flat_multi_range_reader(s, std::move(ms), ranges, cmd.slice, service::get_local_sstable_query_read_priority(),
                    trace_state, mutation_reader::forwarding::no);

            auto compaction_state = make_lw_shared<compact_for_mutation_query_state>(*s, cmd.timestamp, cmd.slice, cmd.row_limit,
                    cmd.partition_limit);

            return do_with(std::move(reader), std::move(compaction_state), [&, accounter = std::move(accounter), timeout] (
                        flat_mutation_reader& reader, lw_shared_ptr<compact_for_mutation_query_state>& compaction_state) mutable {
                auto rrb = reconcilable_result_builder(*reader.schema(), cmd.slice, std::move(accounter));
                return query::consume_page(reader,
                        compaction_state,
                        cmd.slice,
                        std::move(rrb),
                        cmd.row_limit,
                        cmd.partition_limit,
                        cmd.timestamp,
                        timeout).then([&] (std::optional<clustering_key_prefix>&& last_ckey, reconcilable_result&& result) mutable {
                    return make_ready_future<std::optional<clustering_key_prefix>,
                            reconcilable_result,
                            circular_buffer<mutation_fragment>,
                            lw_shared_ptr<compact_for_mutation_query_state>>(std::move(last_ckey), std::move(result), reader.detach_buffer(),
                                    std::move(compaction_state));
                });
            }).then_wrapped([&ctx] (future<std::optional<clustering_key_prefix>, reconcilable_result, circular_buffer<mutation_fragment>,
                    lw_shared_ptr<compact_for_mutation_query_state>>&& result_fut) {
                if (result_fut.failed()) {
                    return make_exception_future<reconcilable_result>(std::move(result_fut.get_exception()));
                }

                auto [last_ckey, result, unconsumed_buffer, compaction_state] = result_fut.get();
                if (!compaction_state->are_limits_reached() && !result.is_short_read()) {
                    return make_ready_future<reconcilable_result>(std::move(result));
                }

                return ctx->save_readers(std::move(unconsumed_buffer), std::move(*compaction_state).detach_state(),
                        std::move(last_ckey)).then_wrapped([result = std::move(result)] (future<>&&) mutable {
                    return make_ready_future<reconcilable_result>(std::move(result));
                });
            });
        });
    });
}

future<foreign_ptr<lw_shared_ptr<reconcilable_result>>, cache_temperature> query_mutations_on_all_shards(
        distributed<database>& db,
        schema_ptr s,
        const query::read_command& cmd,
        const dht::partition_range_vector& ranges,
        tracing::trace_state_ptr trace_state,
        uint64_t max_size,
        db::timeout_clock::time_point timeout) {
    if (cmd.row_limit == 0 || cmd.slice.partition_row_limit() == 0 || cmd.partition_limit == 0) {
        return make_ready_future<foreign_ptr<lw_shared_ptr<reconcilable_result>>, cache_temperature>(
                make_foreign(make_lw_shared<reconcilable_result>()),
                db.local().find_column_family(s).get_global_cache_hit_rate());
    }

    return db.local().get_result_memory_limiter().new_mutation_read(max_size).then([&, s = std::move(s), trace_state = std::move(trace_state),
            timeout] (query::result_memory_accounter accounter) mutable {
        return do_query_mutations(db, s, cmd, ranges, std::move(trace_state), timeout, std::move(accounter)).then_wrapped(
                    [&db, s = std::move(s)] (future<reconcilable_result>&& f) {
            auto& local_db = db.local();
            auto& stats = local_db.get_stats();
            if (f.failed()) {
                ++stats.total_reads_failed;
                return make_exception_future<foreign_ptr<lw_shared_ptr<reconcilable_result>>, cache_temperature>(f.get_exception());
            } else {
                ++stats.total_reads;
                auto result = f.get0();
                stats.short_mutation_queries += bool(result.is_short_read());
                auto hit_rate = local_db.find_column_family(s).get_global_cache_hit_rate();
                return make_ready_future<foreign_ptr<lw_shared_ptr<reconcilable_result>>, cache_temperature>(
                        make_foreign(make_lw_shared<reconcilable_result>(std::move(result))), hit_rate);
            }
        });
    });
}
