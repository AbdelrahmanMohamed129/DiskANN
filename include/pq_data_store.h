#pragma once
#include <memory>
#include "distance.h"
#include "pq.h"
#include "abstract_data_store.h"

namespace diskann {
template <typename data_t>
class PQDataStore : public AbstractDataStore {
  DISKANN_DLLEXPORT PQDataStore(
      uint32_t dim, uint32_t num_pq_chunks,
      std::shared_ptr<Distance<data_t>> distance_fn,
      std::shared_ptr<AbstractPQDistance<data_t>> pq_distance_fn);
  DISKANN_DLLEXPORT ~PQDataStore();

  //Load quantized vectors from a set of files. Here filename is treated
  //as a prefix and the files are assumed to be named with DiskANN 
  //conventions. 
  virtual location_t load(const std::string &file_prefix) override;

  //Save quantized vectors to a set of files whose names start with file_prefix.
  // Currently, the plan is to save the quantized vectors to the quantized
  // vectors file 
  virtual size_t save(const std::string &file_prefix,
                      const location_t num_points) override;

  virtual size_t get_aligned_dim() const override;

  // Populate quantized data from unaligned data using PQ functionality
  virtual void populate_data(const data_t *vectors,
                             const location_t num_pts) override;
  virtual void populate_data(const std::string &filename,
                             const size_t offset) override;

  virtual void extract_data_to_bin(const std::string &filename,
                                   const location_t num_pts) override;

  virtual void get_vector(const location_t i, data_t *target) const override;
  virtual void set_vector(const location_t i,
                          const data_t *const vector) override;
  virtual void prefetch_vector(const location_t loc) override;

  virtual void move_vectors(const location_t old_location_start,
                            const location_t new_location_start,
                            const location_t num_points) override;
  virtual void copy_vectors(const location_t from_loc, const location_t to_loc,
                            const location_t num_points) override;

  virtual float get_distance(const data_t *query,
                             const location_t loc) const override;
  virtual float get_distance(const location_t loc1,
                             const location_t loc2) const override;
  virtual void get_distance(const data_t *query, const location_t *locations,
                            const uint32_t location_count,
                            float *distances, AbstractScratch* scratch_space) const override;

  virtual location_t calculate_medoid() const override;

  virtual Distance<data_t> *get_dist_fn();

  virtual size_t get_alignment_factor() const override;

 protected:
  virtual location_t expand(const location_t new_size) override;
  virtual location_t shrink(const location_t new_size) override;

  virtual location_t load_impl(const std::string &filename);
#ifdef EXEC_ENV_OLS
  virtual location_t load_impl(AlignedFileReader &reader);
#endif

  private:
   uint8_t* _quantized_data = nullptr;
   size_t _num_chunks = 0;

   //REFACTOR TODO: Doing this temporarily before refactoring OPQ into 
  //its own class. Remove later. 
   bool _use_opq;

  Metric _distance_metric;
  AbstractPQDistance<data_t> *_pq_distance_fn = nullptr;
};
}  // namespace diskann