// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <tuple>

#include <omp.h>
#include "filter_utils.h"
#include "index.h"
#include "parameters.h"
#include "utils.h"

namespace diskann
{
/*
 * Using passed in parameters and files generated from step 3,
 * builds a vanilla diskANN index for each label.
 *
 * Each index is saved under the following path:
 *  final_index_path_prefix + "_" + label
 */
template <typename T>
void generate_label_indices(path input_data_path, path final_index_path_prefix, label_set all_labels, unsigned R,
                            unsigned L, float alpha, unsigned num_threads)
{
    diskann::Parameters label_index_build_parameters;
    label_index_build_parameters.Set<unsigned>("R", R);
    label_index_build_parameters.Set<unsigned>("L", L);
    label_index_build_parameters.Set<unsigned>("C", 750);
    label_index_build_parameters.Set<unsigned>("Lf", 0);
    label_index_build_parameters.Set<bool>("saturate_graph", 0);
    label_index_build_parameters.Set<float>("alpha", alpha);
    label_index_build_parameters.Set<unsigned>("num_threads", num_threads);

    std::cout << "Generating indices per label..." << std::endl;
    // for each label, build an index on resp. points
    double total_indexing_time = 0.0, indexing_percentage = 0.0;
    std::cout.setstate(std::ios_base::failbit);
    diskann::cout.setstate(std::ios_base::failbit);
    for (const auto &lbl : all_labels)
    {
        path curr_label_input_data_path(input_data_path + "_" + lbl);
        path curr_label_index_path(final_index_path_prefix + "_" + lbl);

        size_t number_of_label_points, dimension;
        diskann::get_bin_metadata(curr_label_input_data_path, number_of_label_points, dimension);
        diskann::Index<T> index(diskann::Metric::L2, dimension, number_of_label_points, false, false);

        auto index_build_timer = std::chrono::high_resolution_clock::now();
        index.build(curr_label_input_data_path.c_str(), number_of_label_points, label_index_build_parameters);
        std::chrono::duration<double> current_indexing_time =
            std::chrono::high_resolution_clock::now() - index_build_timer;

        total_indexing_time += current_indexing_time.count();
        indexing_percentage += (1 / (double)all_labels.size());
        print_progress(indexing_percentage);

        index.save(curr_label_index_path.c_str());
    }
    std::cout.clear();
    diskann::cout.clear();

    std::cout << "\nDone. Generated per-label indices in " << total_indexing_time << " seconds\n" << std::endl;
}

// for use on systems without writev (i.e. Windows)
template <typename T>
tsl::robin_map<std::string, std::vector<_u32>> generate_label_specific_vector_files_compat(
    path input_data_path, tsl::robin_map<std::string, _u32> labels_to_number_of_points,
    std::vector<label_set> point_ids_to_labels, label_set all_labels)
{
    auto file_writing_timer = std::chrono::high_resolution_clock::now();
    std::ifstream input_data_stream(input_data_path);

    _u32 number_of_points, dimension;
    input_data_stream.read((char *)&number_of_points, sizeof(_u32));
    input_data_stream.read((char *)&dimension, sizeof(_u32));
    const _u32 VECTOR_SIZE = dimension * sizeof(T);
    if (number_of_points != point_ids_to_labels.size())
    {
        std::cerr << "Error: number of points in labels file and data file differ." << std::endl;
        throw;
    }

    tsl::robin_map<std::string, char *> labels_to_vectors;
    tsl::robin_map<std::string, _u32> labels_to_curr_vector;
    tsl::robin_map<std::string, std::vector<_u32>> label_id_to_orig_id;

    for (const auto &lbl : all_labels)
    {
        _u32 number_of_label_pts = labels_to_number_of_points[lbl];
        char *vectors = (char *)malloc(number_of_label_pts * VECTOR_SIZE);
        if (vectors == nullptr)
        {
            throw;
        }
        labels_to_vectors[lbl] = vectors;
        labels_to_curr_vector[lbl] = 0;
        label_id_to_orig_id[lbl].reserve(number_of_label_pts);
    }

    for (_u32 point_id = 0; point_id < number_of_points; point_id++)
    {
        char *curr_vector = (char *)malloc(VECTOR_SIZE);
        input_data_stream.read(curr_vector, VECTOR_SIZE);
        for (const auto &lbl : point_ids_to_labels[point_id])
        {
            char *curr_label_vector_ptr = labels_to_vectors[lbl] + (labels_to_curr_vector[lbl] * VECTOR_SIZE);
            memcpy(curr_label_vector_ptr, curr_vector, VECTOR_SIZE);
            labels_to_curr_vector[lbl]++;
            label_id_to_orig_id[lbl].push_back(point_id);
        }
        free(curr_vector);
    }

    for (const auto &lbl : all_labels)
    {
        path curr_label_input_data_path(input_data_path + "_" + lbl);
        _u32 number_of_label_pts = labels_to_number_of_points[lbl];

        std::ofstream label_file_stream;
        label_file_stream.exceptions(std::ios::badbit | std::ios::failbit);
        label_file_stream.open(curr_label_input_data_path, std::ios_base::binary);
        label_file_stream.write((char *)&number_of_label_pts, sizeof(_u32));
        label_file_stream.write((char *)&dimension, sizeof(_u32));
        label_file_stream.write((char *)labels_to_vectors[lbl], number_of_label_pts * VECTOR_SIZE);

        label_file_stream.close();
        free(labels_to_vectors[lbl]);
    }
    input_data_stream.close();

    std::chrono::duration<double> file_writing_time = std::chrono::high_resolution_clock::now() - file_writing_timer;
    std::cout << "generated " << all_labels.size() << " label-specific vector files for index building in time "
              << file_writing_time.count() << "\n"
              << std::endl;

    return label_id_to_orig_id;
}

/*
 * Manually loads a graph index in from a given file.
 *
 * Returns both the graph index and the size of the file in bytes.
 */
load_label_index_return_values load_label_index(path label_index_path, _u32 label_number_of_points)
{
    std::ifstream label_index_stream;
    label_index_stream.exceptions(std::ios::badbit | std::ios::failbit);
    label_index_stream.open(label_index_path, std::ios::binary);

    _u64 index_file_size, index_num_frozen_points;
    _u32 index_max_observed_degree, index_entry_point;
    const size_t INDEX_METADATA = 2 * sizeof(_u64) + 2 * sizeof(_u32);
    label_index_stream.read((char *)&index_file_size, sizeof(_u64));
    label_index_stream.read((char *)&index_max_observed_degree, sizeof(_u32));
    label_index_stream.read((char *)&index_entry_point, sizeof(_u32));
    label_index_stream.read((char *)&index_num_frozen_points, sizeof(_u64));
    size_t bytes_read = INDEX_METADATA;

    std::vector<std::vector<_u32>> label_index(label_number_of_points);
    _u32 nodes_read = 0;
    while (bytes_read != index_file_size)
    {
        _u32 current_node_num_neighbors;
        label_index_stream.read((char *)&current_node_num_neighbors, sizeof(_u32));
        nodes_read++;

        std::vector<_u32> current_node_neighbors(current_node_num_neighbors);
        label_index_stream.read((char *)current_node_neighbors.data(), current_node_num_neighbors * sizeof(_u32));
        label_index[nodes_read - 1].swap(current_node_neighbors);
        bytes_read += sizeof(_u32) * (current_node_num_neighbors + 1);
    }

    return std::make_tuple(label_index, index_file_size);
}

/*
 * Parses the label datafile, which has comma-separated labels on
 * each line. Line i corresponds to point id i.
 *
 * Returns three objects via std::tuple:
 * 1. map: key is point id, value is vector of labels said point has
 * 2. map: key is label, value is number of points with the label
 * 3. the label universe as a set
 */
parse_label_file_return_values parse_label_file(path label_data_path, std::string universal_label)
{
    std::ifstream label_data_stream(label_data_path);
    std::string line, token;
    unsigned line_cnt = 0;

    // allows us to reserve space for the points_to_labels vector
    while (std::getline(label_data_stream, line))
        line_cnt++;
    label_data_stream.clear();
    label_data_stream.seekg(0, std::ios::beg);

    // values to return
    std::vector<label_set> point_ids_to_labels(line_cnt);
    tsl::robin_map<std::string, _u32> labels_to_number_of_points;
    label_set all_labels;

    std::vector<_u32> points_with_universal_label;
    line_cnt = 0;
    while (std::getline(label_data_stream, line))
    {
        std::istringstream current_labels_comma_separated(line);
        label_set current_labels;

        // get point id
        _u32 point_id = line_cnt;

        // parse comma separated labels
        bool current_universal_label_check = false;
        while (getline(current_labels_comma_separated, token, ','))
        {
            token.erase(std::remove(token.begin(), token.end(), '\n'), token.end());
            token.erase(std::remove(token.begin(), token.end(), '\r'), token.end());

            // if token is empty, there's no labels for the point
            if (token == universal_label)
            {
                points_with_universal_label.push_back(point_id);
                current_universal_label_check = true;
            }
            else
            {
                all_labels.insert(token);
                current_labels.insert(token);
                labels_to_number_of_points[token]++;
            }
        }

        if (current_labels.size() <= 0 && !current_universal_label_check)
        {
            std::cerr << "Error: " << point_id << " has no labels." << std::endl;
            exit(-1);
        }
        point_ids_to_labels[point_id] = current_labels;
        line_cnt++;
    }

    // for every point with universal label, set its label set to all labels
    // also, increment the count for number of points a label has
    for (const auto &point_id : points_with_universal_label)
    {
        point_ids_to_labels[point_id] = all_labels;
        for (const auto &lbl : all_labels)
            labels_to_number_of_points[lbl]++;
    }

    std::cout << "Identified " << all_labels.size() << " distinct label(s) for " << point_ids_to_labels.size()
              << " points\n"
              << std::endl;

    return std::make_tuple(point_ids_to_labels, labels_to_number_of_points, all_labels);
}

template DISKANN_DLLEXPORT void generate_label_indices<float>(path input_data_path, path final_index_path_prefix,
                                                              label_set all_labels, unsigned R, unsigned L, float alpha,
                                                              unsigned num_threads);
template DISKANN_DLLEXPORT void generate_label_indices<uint8_t>(path input_data_path, path final_index_path_prefix,
                                                                label_set all_labels, unsigned R, unsigned L,
                                                                float alpha, unsigned num_threads);
template DISKANN_DLLEXPORT void generate_label_indices<int8_t>(path input_data_path, path final_index_path_prefix,
                                                               label_set all_labels, unsigned R, unsigned L,
                                                               float alpha, unsigned num_threads);

template DISKANN_DLLEXPORT tsl::robin_map<std::string, std::vector<_u32>> generate_label_specific_vector_files_compat<
    float>(path input_data_path, tsl::robin_map<std::string, _u32> labels_to_number_of_points,
           std::vector<label_set> point_ids_to_labels, label_set all_labels);
template DISKANN_DLLEXPORT tsl::robin_map<std::string, std::vector<_u32>> generate_label_specific_vector_files_compat<
    uint8_t>(path input_data_path, tsl::robin_map<std::string, _u32> labels_to_number_of_points,
             std::vector<label_set> point_ids_to_labels, label_set all_labels);
template DISKANN_DLLEXPORT tsl::robin_map<std::string, std::vector<_u32>> generate_label_specific_vector_files_compat<
    int8_t>(path input_data_path, tsl::robin_map<std::string, _u32> labels_to_number_of_points,
            std::vector<label_set> point_ids_to_labels, label_set all_labels);

} // namespace diskann