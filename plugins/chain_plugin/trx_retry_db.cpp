#include <eosio/chain_plugin/trx_retry_db.hpp>

#include <eosio/chain/contract_types.hpp>
#include <eosio/chain/controller.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/member.hpp>


using namespace eosio;
using namespace eosio::chain;
using namespace eosio::chain::literals;
using namespace boost::multi_index;

namespace {

   struct tracked_transaction {
      const transaction_metadata_ptr trx_meta;
      uint32_t                       block_num = 0;
      fc::variant                    trx_trace_v;

      const transaction_id_type& id()const { return trx_meta->id(); }
      fc::time_point_sec expiry()const { return trx_meta->packed_trx()->expiration(); }

      tracked_transaction(const tracked_transaction&) = delete;
      tracked_transaction() = delete;
      tracked_transaction& operator=(const tracked_transaction&) = delete;
      tracked_transaction(tracked_transaction&&) = default;
   };

   struct by_trx_id;
   struct by_expiry;

   using tracked_transaction_index_t = multi_index_container<tracked_transaction,
         indexed_by<
               hashed_unique<tag<by_trx_id>,
                     const_mem_fun<tracked_transaction, const transaction_id_type&, &tracked_transaction::id>, std::hash<transaction_id_type>
               >,
               ordered_non_unique<tag<by_expiry>,
                     const_mem_fun<tracked_transaction, fc::time_point_sec, &tracked_transaction::expiry>
               >
         >
   >;

} // anonymous namespace

namespace eosio::chain_apis {

   struct trx_retry_db_impl {
      explicit trx_retry_db_impl(const chain::controller& controller)
      :controller(controller)
      {}

      /**
       * Store a potentially relevant transaction trace in a short lived cache so that it can be processed if its
       * committed to by a block
       * @param trace
       */
      void cache_transaction_trace( const chain::transaction_trace_ptr& trace ) {
         if( !trace->receipt ) return;
         // include only executed transactions; soft_fail included so that onerror (and any inlines via onerror) are included
         if((trace->receipt->status != chain::transaction_receipt_header::executed &&
             trace->receipt->status != chain::transaction_receipt_header::soft_fail)) {
            return;
         }
         if( chain::is_onblock( *trace )) {
            onblock_trace.emplace( trace );
         } else if( trace->failed_dtrx_trace ) {
            cached_trace_map[trace->failed_dtrx_trace->id] = trace;
         } else {
            auto& idx = tracked_trxs.get<by_trx_id>();
            auto itr = idx.find(trace->id);
            if( itr != idx.end() ) {
               idx.modify( itr, [&trace, &control=controller, &abi_max_time=abi_serializer_max_time]( tracked_transaction& tt ) {
                  tt.block_num = trace->block_num;
                  try {
                     // send_transaction trace output format
                     tt.trx_trace_v = control.to_variant_with_abi( *trace, abi_serializer::create_yield_function( abi_max_time ) );
                  } catch( chain::abi_exception& ) {
                     tt.trx_trace_v = *trace;
                  }
               } );
            }
            cached_trace_map[trace->id] = trace;
         }
      }

      /**
       * Commit a block of transactions to the DB
       * transaction traces need to be in the cache prior to this call
       * @param bsp
       */
      void commit_block(const chain::block_state_ptr& bsp ) {
         // todo

         // drop any unprocessed cached traces
         cached_trace_map.clear();
         onblock_trace.reset();
      }


      /**
       * Convenience aliases
       */
      using cached_trace_map_t = std::map<chain::transaction_id_type, chain::transaction_trace_ptr>;
      using onblock_trace_t = std::optional<chain::transaction_trace_ptr>;

      const chain::controller&    controller;               ///< the controller to read data from
      const fc::microseconds      abi_serializer_max_time;  ///< the maximum time to allow abi_serialization to run
      cached_trace_map_t          cached_trace_map;         ///< temporary cache of uncommitted traces
      onblock_trace_t             onblock_trace;            ///< temporary cache of on_block trace
      tracked_transaction_index_t tracked_trxs;
   };

   trx_retry_db::trx_retry_db( const chain::controller& controller )
   :_impl(std::make_unique<trx_retry_db_impl>(controller))
   {
      //_impl->build_account_query_map();
   }

   trx_retry_db::~trx_retry_db() = default;
   trx_retry_db& trx_retry_db::operator=(trx_retry_db &&) = default;

   void trx_retry_db::cache_transaction_trace( const chain::transaction_trace_ptr& trace ) {
      try {
         _impl->cache_transaction_trace(trace);
      } FC_LOG_AND_DROP(("trx retry cache_transaction_trace ERROR"));
   }

   void trx_retry_db::commit_block(const chain::block_state_ptr& block ) {
      try {
         _impl->commit_block(block);
      } FC_LOG_AND_DROP(("trx retry commit_block ERROR"));
   }


} // namespace eosio::chain_apis
