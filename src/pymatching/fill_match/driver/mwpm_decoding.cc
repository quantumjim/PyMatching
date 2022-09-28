#include "pymatching/fill_match/driver/mwpm_decoding.h"

void pm::fill_bit_vector_from_obs_mask(
        pm::obs_int obs_mask,
        uint8_t *obs_begin_ptr,
        size_t num_observables
        ){
    auto max_obs = sizeof(pm::obs_int) * 8;
    if (num_observables > max_obs)
        throw std::invalid_argument("Too many observables");
    for (size_t i = 0; i < num_observables; i++)
        *(obs_begin_ptr + i) ^= (obs_mask & ((pm::obs_int) 1 << i)) >> i;
}

pm::obs_int pm::bit_vector_to_obs_mask(const std::vector<uint8_t>& bit_vector){
    auto num_observables = bit_vector.size();
    auto max_obs = sizeof(pm::obs_int) * 8;
    if (num_observables > max_obs)
        throw std::invalid_argument("Too many observables");
    pm::obs_int obs_mask = 0;
    for (size_t i = 0; i < num_observables; i++)
        obs_mask ^= bit_vector[i] << i;
    return obs_mask;
}

pm::Mwpm pm::detector_error_model_to_mwpm(
    const stim::DetectorErrorModel& detector_error_model, pm::weight_int num_distinct_weights) {
    auto probability_graph = pm::detector_error_model_to_probability_graph(detector_error_model);
    if (probability_graph.num_observables > sizeof(pm::obs_int) * 8) {
        auto mwpm = pm::Mwpm(
                pm::GraphFlooder(probability_graph.to_matching_graph(num_distinct_weights)),
                pm::SearchFlooder(probability_graph.to_search_graph(num_distinct_weights))
                        );
        mwpm.flooder.sync_negative_weight_observables_and_detection_events();
        return mwpm;
    } else {
        auto mwpm = pm::Mwpm(pm::GraphFlooder(
                probability_graph.to_matching_graph(num_distinct_weights)
        ));
        mwpm.flooder.sync_negative_weight_observables_and_detection_events();
        return mwpm;
    }
}


void process_timeline_until_completion(pm::Mwpm& mwpm, const std::vector<uint64_t>& detection_events) {
    if (!mwpm.flooder.queue.empty()) {
        throw std::invalid_argument("!mwpm.flooder.queue.empty()");
    }
    mwpm.flooder.queue.cur_time = 0;

    if (mwpm.flooder.negative_weight_detection_events.empty()){

        // Just add detection events if graph has no negative weights
        for (auto& detection : detection_events) {
            if (detection >= mwpm.flooder.graph.nodes.size())
                throw std::invalid_argument("Detection event index too large");
            mwpm.create_detection_event(&mwpm.flooder.graph.nodes[detection]);
        }

    } else {

        // First mark nodes with negative weight detection events
        for (auto& det : mwpm.flooder.negative_weight_detection_events){
            mwpm.flooder.graph.nodes[det].radius_of_arrival = 1;
        }

        // Now add detection events for unmarked nodes
        for (auto& detection : detection_events) {
            if (detection >= mwpm.flooder.graph.nodes.size())
                throw std::invalid_argument("Detection event index too large");
            if (!mwpm.flooder.graph.nodes[detection].radius_of_arrival){
                mwpm.create_detection_event(&mwpm.flooder.graph.nodes[detection]);
            } else {
                // Unmark node
                mwpm.flooder.graph.nodes[detection].radius_of_arrival = 0;
            }
        }

        for (auto& det : mwpm.flooder.negative_weight_detection_events){
            if (mwpm.flooder.graph.nodes[det].radius_of_arrival){
                // Add a detection event if the node is still marked
                mwpm.flooder.graph.nodes[det].radius_of_arrival = 0;
                mwpm.create_detection_event(&mwpm.flooder.graph.nodes[det]);
            }
        }

    }

    while (true) {
        auto event = mwpm.flooder.run_until_next_mwpm_notification();
        if (event.event_type == pm::NO_EVENT)
            break;
        mwpm.process_event(event);
    }
}


pm::MatchingResult shatter_blossoms_for_all_detection_events_and_extract_obs_mask_and_weight(
        pm::Mwpm& mwpm, const std::vector<uint64_t>& detection_events) {
    pm::MatchingResult res;
    for (auto& i : detection_events) {
        if (mwpm.flooder.graph.nodes[i].region_that_arrived)
            res += mwpm.shatter_blossom_and_extract_matches(mwpm.flooder.graph.nodes[i].region_that_arrived_top);
    }
    return res;
}


pm::MatchingResult pm::decode_detection_events_for_up_to_64_observables(pm::Mwpm& mwpm, const std::vector<uint64_t>& detection_events) {
    process_timeline_until_completion(mwpm, detection_events);
    auto res = shatter_blossoms_for_all_detection_events_and_extract_obs_mask_and_weight(mwpm, detection_events);
    res.obs_mask ^= mwpm.flooder.negative_weight_obs_mask;
    return res;
}

void pm::decode_detection_events(
        pm::Mwpm& mwpm, const std::vector<uint64_t>& detection_events,
        uint8_t *obs_begin_ptr,
        pm::total_weight_int & weight) {
    size_t num_observables = mwpm.flooder.graph.num_observables;
    process_timeline_until_completion(mwpm, detection_events);

    if (num_observables > sizeof(pm::obs_int) * 8) {

        mwpm.flooder.match_edges.clear();
        for (auto& i : detection_events) {
            if (mwpm.flooder.graph.nodes[i].region_that_arrived)
                mwpm.shatter_blossom_and_extract_match_edges(
                        mwpm.flooder.graph.nodes[i].region_that_arrived_top,
                        mwpm.flooder.match_edges
                        );
        }
        mwpm.extract_paths_from_match_edges(mwpm.flooder.match_edges, obs_begin_ptr,weight);

        // XOR negative weight observables
        for (auto& obs : mwpm.flooder.negative_weight_observables)
            *(obs_begin_ptr + obs) ^= 1;
        // Add negative weight sum to blossom solution weight
        weight += mwpm.flooder.negative_weight_sum;

    } else {

        pm::MatchingResult bit_packed_res = shatter_blossoms_for_all_detection_events_and_extract_obs_mask_and_weight(
                mwpm, detection_events
                );
        // XOR in negative weight observable mask
        bit_packed_res.obs_mask ^= mwpm.flooder.negative_weight_obs_mask;
        // Translate observable mask into bit vector
        fill_bit_vector_from_obs_mask(bit_packed_res.obs_mask, obs_begin_ptr,
                                      num_observables);
        // Add negative weight sum to blossom solution weight
        weight = bit_packed_res.weight + mwpm.flooder.negative_weight_sum;
    }

}
