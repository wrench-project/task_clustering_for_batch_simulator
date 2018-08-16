/**
 * Copyright (c) 2017. The WRENCH Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <Util/WorkflowUtil.h>
#include "ClusteredJob.h"

XBT_LOG_NEW_DEFAULT_CATEGORY(clustered_job, "Log category for Clustered Job");


namespace wrench {

    void ClusteredJob::addTask(WorkflowTask *task) {
      this->tasks.push_back(task);
    }

    bool ClusteredJob::isTaskOK(wrench::WorkflowTask *task) {
      if (task->getState() == wrench::WorkflowTask::READY) {
        return true;
      }
      for (auto p : task->getWorkflow()->getTaskParents(task)) {
        if ((p->getState() != wrench::WorkflowTask::COMPLETED) &&
                (std::find(this->tasks.begin(), this->tasks.end(), p) == this->tasks.end())) {
          return false;
        }
      }
      return true;
    }

    bool ClusteredJob::isReady() {
      for (auto t : this->tasks) {
        if (not isTaskOK(t)) {
          return false;
        }
      }
      return true;
    }

    void ClusteredJob::setNumNodes(unsigned long num_nodes, bool based_on_queue_wait_time_prediction) {
      this->num_nodes = num_nodes;
      this->num_nodes_based_on_queue_wait_time_predictions = based_on_queue_wait_time_prediction;
    }

    unsigned long ClusteredJob::getNumTasks() {
      return this->tasks.size();
    }

    unsigned long ClusteredJob::getNumNodes() {
      return this->num_nodes;
    }

    std::vector<wrench::WorkflowTask *> ClusteredJob::getTasks() {
      return this->tasks;
    }

    double ClusteredJob::estimateMakespan(double core_speed) {
      if (this->num_nodes  == 0) {
        throw std::runtime_error("estimateMakespan(): Cannot estimate makespan with 0 nodes!");
      }

      return WorkflowUtil::estimateMakespan(this->tasks, this->num_nodes, core_speed);
    }


    double ClusteredJob::estimateMakespan(double core_speed, unsigned long n) {
      if (n  == 0) {
        throw std::runtime_error("estimateMakespan(): Cannot estimate makespan with 0 nodes!");
      }

      return WorkflowUtil::estimateMakespan(this->tasks, n, core_speed);
    }

    bool ClusteredJob::isNumNodesBasedOnQueueWaitTimePrediction() {
      return this->num_nodes_based_on_queue_wait_time_predictions;
    }


    unsigned long ClusteredJob::computeBestNumNodesBasedOnQueueWaitTimePredictions(unsigned long max_num_nodes, double core_speed, BatchService *batch_service) {

      std::cerr << "HERE: max_num_nodes = " << max_num_nodes << "\n";
      // Build job configurations
      unsigned long real_max_num_nodes = std::min(this->getNumTasks(), max_num_nodes);
      std::cerr << "real_max_num_nodes" << real_max_num_nodes << "\n";


      std::string job_id_prefix = "my_tentative_job";
      std::set<std::tuple<std::string,unsigned int,unsigned int, double>> set_of_job_configurations;
      for (unsigned int n = 1; n <= real_max_num_nodes; n++) {
        double walltime_seconds = this->estimateMakespan(core_speed, n);
        walltime_seconds *= EXECUTION_TIME_FUDGE_FACTOR;
        std::tuple<std::string, unsigned int, unsigned int, double> my_job =
                std::make_tuple(job_id_prefix + "_" + std::to_string(Simulator::sequence_number++),
                                n, 1, walltime_seconds);
        set_of_job_configurations.insert(my_job);
      }


//      for (auto c : set_of_job_configurations) {
//        std::cerr << "---> " << std::get<0>(c) << " " << std::get<1>(c) << " " << std::get<2>(c) << " " << std::get<3>(c) << "\n";
//      }

      // Get estimates
      WRENCH_INFO("Getting Queue Wait Time estimates for %ld job configurations...", set_of_job_configurations.size());
      std::map<std::string,double> jobs_estimated_start_times;
      try {
        jobs_estimated_start_times = batch_service->getStartTimeEstimates(set_of_job_configurations);
      } catch (wrench::WorkflowExecutionException &e) {
        throw std::runtime_error(std::string("Couldn't acquire queue wait time predictions: ") + e.what());
      }

      if (jobs_estimated_start_times.size() != real_max_num_nodes) {
        throw std::runtime_error("Was expecting " + std::to_string(real_max_num_nodes) + " wait time estimates but got " +
                                 std::to_string(jobs_estimated_start_times.size()) + "instead!");
      }
      // Find out the best
      unsigned long best_num_nodes = ULONG_MAX;
      double best_finish_time = -1.0;
      for (auto estimate : jobs_estimated_start_times) {
        std::string job_id = estimate.first;
        double start_time = estimate.second;
        double makespan = -1.0;
        unsigned long num_nodes;

        // find the job configuration in the set (inefficient)
        for (auto jc : set_of_job_configurations) {
          if (std::get<0>(jc) == job_id) {
            makespan = std::get<3>(jc);
            num_nodes = std::get<1>(jc);
          }
        }
        if (makespan < 0) {
          throw std::runtime_error("Fatal error when looking at queue wait time predictions!");
        }

        double finish_time = start_time + makespan;

        WRENCH_INFO("  - QWTE with %lu node: start time=%lf + makespan=%lf  =  finishtime=%lf", num_nodes, start_time, makespan, finish_time);

        if ((finish_time < best_finish_time) or (best_finish_time < 0.0)) {
          best_finish_time = finish_time;
          best_num_nodes = num_nodes;
        }
      }

      WRENCH_INFO("Opted to use %lu compute nodes!", best_num_nodes);


      return best_num_nodes;
    }
};