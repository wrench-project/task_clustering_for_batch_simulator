/**
 * Copyright (c) 2017. The WRENCH Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <wrench-dev.h>
#include <Util/WorkflowUtil.h>
#include <Simulator.h>
#include "ZhangFixedWMS.h"
#include <Util/ProxyWMS.h>
#include <Util/PlaceHolderJob.h>
#include <assert.h>

XBT_LOG_NEW_DEFAULT_CATEGORY(zhang_fixed_wms,
                             "Log category for Zhang Fixed WMS");

#define EXECUTION_TIME_FUDGE_FACTOR 1.1

namespace wrench {

    class Simulator;

    static int sequence = 0;

    ZhangFixedWMS::ZhangFixedWMS(Simulator *simulator, std::string hostname, bool overlap, bool plimit,
                                 std::shared_ptr<BatchComputeService> batch_service) :
            WMS(nullptr, nullptr, {batch_service}, {}, {}, nullptr, hostname, "clustering_wms") {
        this->simulator = simulator;
        this->overlap = overlap;
        this->plimit = plimit;
        this->batch_service = batch_service;
        this->pending_placeholder_job = nullptr;
        this->individual_mode = false;
    }

    int ZhangFixedWMS::main() {

        TerminalOutput::setThisProcessLoggingColor(TerminalOutput::COLOR_WHITE);

        this->checkDeferredStart();

        // Find out core speed on the batch service
        this->core_speed = (*(this->batch_service->getCoreFlopRate().begin())).second;
        // Find out #hosts on the batch service
        this->number_of_hosts = this->batch_service->getNumHosts();

        // Create a job manager
        this->job_manager = this->createJobManager();

        this->proxyWMS = new ProxyWMS(this->getWorkflow(), this->job_manager, this->batch_service);

        while (not this->getWorkflow()->isDone()) {

            // Submit a pilot job (if needed)
            applyGroupingHeuristic();

            this->waitForAndProcessNextEvent();

        }

        std::cout << "#SPLITS=" << this->number_of_splits << "\n";

        return 0;
    }

    void ZhangFixedWMS::applyGroupingHeuristic() {

//      WRENCH_INFO("APPLYING GROUPING HEURISTIC");

        // Don't schedule a pilot job if one is pending
        if (this->pending_placeholder_job) {
            return;
        }

        // Don't schedule a pilot job if we're in individual mode
        if (this->individual_mode) {
            return;
        }

        // Don't schedule a pilot job is overlap = false and anything is running
        if ((not this->overlap) and (not this->running_placeholder_jobs.empty())) {
            return;
        }

        unsigned long start_level = getStartLevel();
        unsigned long end_level = this->getWorkflow()->getNumLevels() - 1;

        // Nothing to do?
        if (start_level > end_level) {
            return;
        }

        // peelLevel() (Fig. 4 in the paper)

        // See if we can do better by grouping (Fig. 5 in the paper)
        // return params: wait_time, makespan, end_level
        std::tuple<double, double, unsigned long> partial_dag = groupLevels(start_level, end_level);
        double partial_dag_wait_time = std::get<0>(partial_dag);
        double partial_dag_makespan = std::get<1>(partial_dag);
        unsigned long partial_dag_end_level = std::get<2>(partial_dag);

        // calculate the runtime of entire DAG
        // maximum parallelism, but never more than total number of nodes
        unsigned long max_parallelism = maxParallelism(start_level, end_level);
        double runtime_all = WorkflowUtil::estimateMakespan(
                this->getWorkflow()->getTasksInTopLevelRange(start_level, end_level),
                max_parallelism, this->core_speed);
        double wait_time_all = this->proxyWMS->estimateWaitTime(max_parallelism, runtime_all,
                                                                this->simulation->getCurrentSimulatedDate(), &sequence);
        std::cout << "PARTIAL END LEVEL: " << partial_dag_end_level << " END LEVEL: " << end_level << std::endl;
        assert(partial_dag_end_level <= end_level);

        if (partial_dag_end_level == end_level) {
            if (wait_time_all > runtime_all * 2.0) {
                // submit remaining dag as 1 job per task
                this->individual_mode = true;
                std::cout << "Switching to individual mode!" << std::endl;
            } else {
                // submit remaining dag as 1 job
            }
        } else {
            std::cout << "Splitting @ end level = " << partial_dag_end_level << std::endl;
            number_of_splits++;
        }

        if (this->individual_mode) { WRENCH_INFO("GROUPING: INDIVIDUAL");
        } else { WRENCH_INFO("GROUPING: %ld-%ld",
                             start_level, end_level);
        }

        if (not individual_mode) {
            // recalculate parallelism for partial dag
            unsigned long parallelism = maxParallelism(start_level, partial_dag_end_level);
            std::cout << "Nodes: " << parallelism << std::endl;
            // TODO this is not right, makespan needs to be recalculated
            this->pending_placeholder_job = this->proxyWMS->createAndSubmitPlaceholderJob(
                    partial_dag_makespan,
                    parallelism,
                    start_level,
                    partial_dag_end_level);
        } else { WRENCH_INFO("Switching to individual mode!");
            // Submit all READY tasks as individual jobs
            this->proxyWMS->submitAllOneJobPerTask(this->core_speed);
        }

    }

    void ZhangFixedWMS::processEventPilotJobStart(std::shared_ptr<PilotJobStartedEvent> e) {

        // Update queue waiting time
        this->simulator->total_queue_wait_time +=
                this->simulation->getCurrentSimulatedDate() - e->pilot_job->getSubmitDate();

        // Just for kicks, check it was the pending one
        WRENCH_INFO("Got a Pilot Job Start event: %s", e->pilot_job->getName().c_str());
        if (this->pending_placeholder_job == nullptr) {
            throw std::runtime_error("Fatal Error: couldn't find a placeholder job for a pilob job that just started");
        }
//      WRENCH_INFO("Got a Pilot Job Start event e->pilot_job = %ld, this->pending->pilot_job = %ld (%s)",
//                  (unsigned long) e->pilot_job,
//                  (unsigned long) this->pending_placeholder_job->pilot_job,
//                  this->pending_placeholder_job->pilot_job->getName().c_str());

        if (e->pilot_job != this->pending_placeholder_job->pilot_job) {

            WRENCH_INFO("Must be for a placeholder I already cancelled... nevermind");
            return;
        }

        PlaceHolderJob *placeholder_job = this->pending_placeholder_job;

        // Move it to running
        this->running_placeholder_jobs.insert(placeholder_job);
        this->pending_placeholder_job = nullptr;

        // Submit all ready tasks to it each in its standard job
        std::string output_string = "";
        for (auto task : placeholder_job->tasks) {
            if (task->getState() == WorkflowTask::READY) {
                StandardJob *standard_job = this->job_manager->createStandardJob(task, {});
                output_string += " " + task->getID();

                WRENCH_INFO("Submitting task %s as part of placeholder job %ld-%ld",
                            task->getID().c_str(), placeholder_job->start_level, placeholder_job->end_level);
                this->job_manager->submitJob(standard_job, placeholder_job->pilot_job->getComputeService());
            }
        }

        // Re-submit a pilot job so as to overlap execution of job n with waiting of job n+1
        this->applyGroupingHeuristic();

    }

    void ZhangFixedWMS::processEventPilotJobExpiration(std::shared_ptr<PilotJobExpiredEvent> e) {
        std::cout << "JOB EXPIRATION!!!" << std::endl;

        // Find the placeholder job
        PlaceHolderJob *placeholder_job = nullptr;
        for (auto ph : this->running_placeholder_jobs) {
            if (ph->pilot_job == e->pilot_job) {
                placeholder_job = ph;
                break;
            }
        }
        if (placeholder_job == nullptr) {
            throw std::runtime_error("Got a pilot job expiration, but no matching placeholder job found");
        }

        // Remove it from the list of running pilot jobs
        this->running_placeholder_jobs.erase(placeholder_job);

        WRENCH_INFO("Got a pilot job expiration for a placeholder job that deals with levels %ld-%ld (%s)",
                    placeholder_job->start_level, placeholder_job->end_level,
                    placeholder_job->pilot_job->getName().c_str());
        // Check if there are unprocessed tasks
        bool unprocessed = false;
        for (auto t : placeholder_job->tasks) {
            if (t->getState() != WorkflowTask::COMPLETED) {
                unprocessed = true;
                break;
            }
        }

//      double wasted_node_seconds = e->pilot_job->getNumHosts() * e->pilot_job->getDuration();
        unsigned long num_used_nodes;
        sscanf(e->pilot_job->getServiceSpecificArguments()["-N"].c_str(), "%lu", &num_used_nodes);
        unsigned long num_used_minutes;
        sscanf(e->pilot_job->getServiceSpecificArguments()["-t"].c_str(), "%lu", &num_used_minutes);
        double wasted_node_seconds = 60.0 * num_used_minutes * num_used_nodes;
        for (auto t : placeholder_job->tasks) {
            if (t->getState() == WorkflowTask::COMPLETED) {
                wasted_node_seconds -= t->getFlops() / this->core_speed;
            }
        }
        this->simulator->wasted_node_seconds += wasted_node_seconds;

        if (not unprocessed) { // Nothing to do
            WRENCH_INFO("This placeholder job has no unprocessed tasks. great.");
            return;
        }

        this->simulator->num_pilot_job_expirations_with_remaining_tasks_to_do++;

        WRENCH_INFO("This placeholder job has unprocessed tasks");

        // Cancel pending pilot job if any
        if (this->pending_placeholder_job) { WRENCH_INFO(
                    "Canceling pending placeholder job (placeholder=%ld,  pilot_job=%ld / %s",
                    (unsigned long) this->pending_placeholder_job,
                    (unsigned long) this->pending_placeholder_job->pilot_job,
                    this->pending_placeholder_job->pilot_job->getName().c_str());
            this->job_manager->terminateJob(this->pending_placeholder_job->pilot_job);
            this->pending_placeholder_job = nullptr;
        }

        // Cancel running pilot jobs if none of their tasks has started

        std::set<PlaceHolderJob *> to_remove;
        for (auto ph : this->running_placeholder_jobs) {
            bool started = false;
            for (auto task : ph->tasks) {
                if (task->getState() != WorkflowTask::NOT_READY) {
                    started = true;
                }
            }
            if (not started) { WRENCH_INFO("Canceling running placeholder job that handled levels %ld-%ld because none"
                                           "of its tasks has started (%s)", ph->start_level, ph->end_level,
                                           ph->pilot_job->getName().c_str());
                try {
                    this->job_manager->terminateJob(ph->pilot_job);
                } catch (WorkflowExecutionException &e) {
                    // ignore (likely already dead!)
                }
                to_remove.insert(ph);
            }
        }

        for (auto ph : to_remove) {
            this->running_placeholder_jobs.erase(ph);
        }

        // Make decisions again
        applyGroupingHeuristic();

    }

    void ZhangFixedWMS::processEventStandardJobCompletion(std::shared_ptr<StandardJobCompletedEvent> e) {

        WorkflowTask *completed_task = e->standard_job->tasks[0]; // only one task per job

        WRENCH_INFO("Got a standard job completion for task %s", completed_task->getID().c_str());

        this->simulator->used_node_seconds += completed_task->getFlops() / this->core_speed;

        // Find the placeholder job this task belongs to
        PlaceHolderJob *placeholder_job = nullptr;
        for (auto ph : this->running_placeholder_jobs) {
            for (auto task : ph->tasks) {
                if (task == completed_task) {
                    placeholder_job = ph;
                    break;
                }
            }
        }

        if ((placeholder_job == nullptr) and (not this->individual_mode)) {
            throw std::runtime_error("Got a task completion, but couldn't find a placeholder for the task, "
                                     "and we're not in individual mode");
        }

        if (placeholder_job != nullptr) {

            // Terminate the pilot job in case all its tasks are done
            bool all_tasks_done = true;
            for (auto t : placeholder_job->tasks) {
                if (t->getState() != WorkflowTask::COMPLETED) {
                    all_tasks_done = false;
                    break;
                }
            }
            if (all_tasks_done) {
                // Update the wasted no seconds metric
                double first_task_start_time = DBL_MAX;
                for (auto const &t : placeholder_job->tasks) {
                    if (t->getStartDate() < first_task_start_time) {
                        first_task_start_time = t->getStartDate();
                    }
                }
                int num_requested_nodes = stoi(placeholder_job->pilot_job->getServiceSpecificArguments()["-N"]);
                double job_duration = this->simulation->getCurrentSimulatedDate() - first_task_start_time;
                double wasted_node_seconds = num_requested_nodes * job_duration;
                for (auto const &t : placeholder_job->tasks) {
//                    this->simulator->used_node_seconds += t->getFlops() / this->core_speed;
                    wasted_node_seconds -= t->getFlops() / this->core_speed;
                }

                this->simulator->wasted_node_seconds += wasted_node_seconds;

                WRENCH_INFO("All tasks are completed in this placeholder job, so I am terminating it (%s)",
                            placeholder_job->pilot_job->getName().c_str());
                try { WRENCH_INFO("TERMINATING A PILOT JOB");
                    this->job_manager->terminateJob(placeholder_job->pilot_job);
                } catch (WorkflowExecutionException &e) {
                    // ignore
                }
                this->running_placeholder_jobs.erase(placeholder_job);
            }


        }

        // Start all newly ready tasks that depended on the completed task, IN ANY PLACEHOLDER
        // This shouldn't happen in individual mode, but can't hurt
        std::vector<WorkflowTask *> children = this->getWorkflow()->getTaskChildren(completed_task);
        for (auto ph : this->running_placeholder_jobs) {
            for (auto task : ph->tasks) {
                if ((std::find(children.begin(), children.end(), task) != children.end()) and
                    (task->getState() == WorkflowTask::READY)) {
                    StandardJob *standard_job = this->job_manager->createStandardJob(task, {});WRENCH_INFO(
                            "Submitting task %s  as part of placeholder job %ld-%ld",
                            task->getID().c_str(), placeholder_job->start_level, placeholder_job->end_level);
                    this->job_manager->submitJob(standard_job, ph->pilot_job->getComputeService());
                }
            }
        }

        if (this->individual_mode) {
            for (auto task : this->getWorkflow()->getTasks()) {
                if (task->getState() == WorkflowTask::State::READY) {
                    StandardJob *standard_job = this->job_manager->createStandardJob(task, {});WRENCH_INFO(
                            "Submitting task %s individually!",
                            task->getID().c_str());
                    std::map<std::string, std::string> service_specific_args;
                    double requested_execution_time =
                            (task->getFlops() / this->core_speed) * EXECUTION_TIME_FUDGE_FACTOR;
                    service_specific_args["-N"] = "1";
                    service_specific_args["-c"] = "1";
                    service_specific_args["-t"] = std::to_string(1 + ((unsigned long) requested_execution_time) / 60);
                    this->job_manager->submitJob(standard_job, this->batch_service, service_specific_args);
                }
            }
        }


    }

    void ZhangFixedWMS::processEventStandardJobFailure(std::shared_ptr<StandardJobFailedEvent> e) {
        WRENCH_INFO("Got a standard job failure event for task %s -- IGNORING THIS",
                    e->standard_job->tasks[0]->getID().c_str());
    }

    // Compute my start level first as the first level that's not fully completed
    unsigned long ZhangFixedWMS::getStartLevel() {
        unsigned long start_level = 0;
        for (unsigned long i = 0; i < this->getWorkflow()->getNumLevels(); i++) {
            std::vector<WorkflowTask *> tasks_in_level = this->getWorkflow()->getTasksInTopLevelRange(i, i);
            bool all_completed = true;
            for (auto task : tasks_in_level) {
                if (task->getState() != WorkflowTask::State::COMPLETED) {
                    all_completed = false;
                }
            }
            if (all_completed) {
                start_level = i + 1;
            }
        }

        for (auto ph : this->running_placeholder_jobs) {
            start_level = 1 + std::max<unsigned long>(start_level, ph->end_level);
        }

        return start_level;
    }

    // Zhang is supposed to fail automatically if number of tasks > number of hosts
    // Just return max hosts to avoid failure for now
    unsigned long ZhangFixedWMS::maxParallelism(unsigned long start_level, unsigned long end_level) {
        if (this->plimit) {
            // Implement strict zhang application here...
        }
        unsigned long parallelism = 0;
        for (unsigned long i = start_level; i <= end_level; i++) {
            unsigned long num_tasks_in_level = this->getWorkflow()->getTasksInTopLevelRange(i, i).size();
            parallelism = std::max<unsigned long>(parallelism, num_tasks_in_level);
        }

        return std::min<unsigned long>(parallelism, this->number_of_hosts);
    }

    // return params: wait_time, run_time, end_level
    std::tuple<double, double, unsigned long>
    ZhangFixedWMS::groupLevels(unsigned long start_level, unsigned long end_level) {

        // runtime of currently running job
        double parent_runtime = ProxyWMS::findMaxDuration(this->running_placeholder_jobs);

        std::cout << "PARENT RUNTIME: " << parent_runtime << std::endl;

        bool giant = true;

        // Start comparing from 2nd level
        // unsigned long candidate_end_level = start_level + 1;
        unsigned long candidate_end_level = start_level;

        double best_so_far_wait_time = DBL_MAX;
        double best_so_far_run_time = 0;
        double best_so_far_leeway = 0;

        // Switch to "<=" if not using entire DAG as 1st comparison
        while (candidate_end_level < end_level) {

            std::cout << "CANDIDATE END LEVEL: " << candidate_end_level << std::endl;

            // Calculate the # nodes and runtime of the current grouping
            unsigned long num_nodes = maxParallelism(start_level, candidate_end_level);

            double run_time =  WorkflowUtil::estimateMakespan(
                    this->getWorkflow()->getTasksInTopLevelRange(start_level, candidate_end_level),
                    num_nodes, this->core_speed);
            double wait_time = this->proxyWMS->estimateWaitTime(num_nodes, run_time,
                                                                   this->simulation->getCurrentSimulatedDate(),
                                                                   &sequence);
            double leeway = 0;
            if (wait_time < parent_runtime) {  // We need a non-zero leeway
                leeway = parent_runtime - wait_time;
                while ((leeway > 1) and
                       ((wait_time = this->proxyWMS->estimateWaitTime(num_nodes, run_time + leeway / 2,
                                                                         this->simulation->getCurrentSimulatedDate(),
                                                                         &sequence)) > parent_runtime)) {
                    leeway /= 2.0;
                }
            }

            std::cout << "runtime: " << run_time << std::endl;
            std::cout << "leeway: " << leeway << std::endl;
            std::cout << "wait_time: " << wait_time << std::endl;

            if (giant) {
                // zhang deems this grouping as invalid, so we pretend we didn't see it...
                // TODO - I wonder if this is why they use all as original check, since what if the first level doesn't
                // meet this standard. we can still default to whole DAG rather than keep first level as baseline
                if (wait_time > run_time) {
                    candidate_end_level++;
                    continue;
                }
            }
            giant = false;

            if ((wait_time / run_time) > (best_so_far_wait_time / best_so_far_run_time)) {
                std::cout << "GOT WORSE\n";
                break;
            }

            best_so_far_wait_time = wait_time;
            best_so_far_run_time = run_time;
            best_so_far_leeway = leeway;

            candidate_end_level++;
        }

        // What if breaks from loop on first level check??
        if (giant || (candidate_end_level == start_level)) {
            // Recalculate leeway needed for entire dag
            unsigned long max_parallelism = maxParallelism(start_level, end_level);
            double runtime_all = WorkflowUtil::estimateMakespan(
                    this->getWorkflow()->getTasksInTopLevelRange(start_level, end_level),
                    max_parallelism, this->core_speed);
            double wait_time_all = this->proxyWMS->estimateWaitTime(max_parallelism, runtime_all,
                                                                     this->simulation->getCurrentSimulatedDate(),
                                                                     &sequence);
            double leeway_all = std::max<double>(0, parent_runtime - wait_time_all);

            best_so_far_run_time = runtime_all;
            best_so_far_wait_time = wait_time_all;
            best_so_far_leeway = leeway_all;
            candidate_end_level = end_level + 1;
        }

        // return partial dag
        return std::make_tuple(best_so_far_wait_time,
                               (best_so_far_run_time + best_so_far_leeway),
                               candidate_end_level - 1);
    }

};
