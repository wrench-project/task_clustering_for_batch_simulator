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
#include "TestClusteringWMS.h"
#include "TestPlaceHolderJob.h"

XBT_LOG_NEW_DEFAULT_CATEGORY(test_clustering_wms,
                             "Log category for Test Clustering WMS");

#define EXECUTION_TIME_FUDGE_FACTOR 1.1

namespace wrench {

    class Simulator;

    static double parent_runtime = 0;

    TestClusteringWMS::TestClusteringWMS(Simulator *simulator, std::string hostname, bool overlap, bool plimit,
                                         double waste_bound, double beat_bound,
                                         std::shared_ptr<BatchComputeService> batch_service) :
            WMS(nullptr, nullptr, {batch_service}, {}, {}, nullptr, hostname, "clustering_wms") {
        this->simulator = simulator;
        this->overlap = overlap;
        this->plimit = plimit;
        this->waste_bound = waste_bound;
        this->batch_service = batch_service;
        this->pending_placeholder_job = nullptr;
        this->individual_mode = false;
        this->beat_bound = beat_bound;
    }

    int TestClusteringWMS::main() {


        TerminalOutput::setThisProcessLoggingColor(TerminalOutput::COLOR_WHITE);

        this->checkDeferredStart();

        // Find out core speed on the batch service
        this->core_speed = (*(this->batch_service->getCoreFlopRate().begin())).second;
        // Find out #hosts on the batch service
        this->number_of_hosts = this->batch_service->getNumHosts();

        // Create a job manager
        this->job_manager = this->createJobManager();

        while (not this->getWorkflow()->isDone()) {

            WorkflowUtil::printRAM();
            // Submit a pilot job (if needed)
            applyGroupingHeuristic();

            this->waitForAndProcessNextEvent();

        }
        std::cout << "#SPLITS=" << this->number_of_splits << "\n";
        return 0;
    }


    /**
    *
    */
    void TestClusteringWMS::applyGroupingHeuristic() {

        WRENCH_INFO("APPLYING GROUPING HEURISTIC");

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

        WRENCH_INFO("STARTING TO COMPUTE STUFF");
        // Compute my start level first as the first level that's not fully completed
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


        WRENCH_INFO("NUM RUNNING PLACE_HOLDER JOBS = %ld", this->running_placeholder_jobs.size());
        for (auto ph : this->running_placeholder_jobs) { WRENCH_INFO("RUNNING PLACEHOLDER JOB: %lu-%lu",
                                                                     ph->start_level, ph->end_level);
            start_level = 1 + std::max<unsigned long>(start_level, ph->end_level);
        }

        WRENCH_INFO("START LEVEL = %lu", start_level);
        // Nothing to do?
        if (start_level >= this->getWorkflow()->getNumLevels()) {
            return;
        }

        unsigned long num_levels = this->getWorkflow()->getNumLevels();
        unsigned long end_level = num_levels - 1;
        // start filling from start level as index
        std::tuple<double, double, unsigned long> time_estimates[num_levels][2];
        for (unsigned long i = start_level; i < num_levels; i++) {
            std::tuple<double, double, unsigned long> wait_run_par = computeBestNumHosts(start_level, i);
            time_estimates[i][0] = wait_run_par;
        }

        for (unsigned long i = start_level; i < num_levels; i++) {
            std::tuple<double, double, unsigned long> wait_run_par = computeBestNumHosts(i, end_level);
            time_estimates[i][1] = wait_run_par;
        }

        std::tuple<double, double, unsigned long> entire_workflow = time_estimates[start_level][1];
        unsigned long requested_parallelism = std::get<2>(entire_workflow);
        double estimated_wait_time = std::get<0>(entire_workflow);
        double requested_execution_time = std::get<1>(entire_workflow);
        double best_total_time = estimated_wait_time + requested_execution_time;

        // std::cerr << "Entire Workflow Stats " << "Start: " << start_level << " End: " << end_level << std::endl;
        // std::cerr << "NODES: " << requested_parallelism << " Wait: " << estimated_wait_time << " Runtime: "
        // << requested_execution_time << std::endl;

        unsigned long partial_dag_end_level = end_level;

        // Apply henri's grouping heuristic
        for (int i = (int) start_level; i < (int) num_levels - 1; i++) {
            std::tuple<double, double, unsigned long> start_to_split = time_estimates[i][0];
            std::tuple<double, double, unsigned long> rest = time_estimates[i + 1][1];
            double wait_one = std::get<0>(start_to_split);
            double run_one = std::get<1>(start_to_split);
            double wait_two = std::get<0>(rest);
            double run_two = std::get<1>(rest);

            // TODO - implement binary search for leeway
            // TODO - use generic placeholder && fix parent_runtime stuff
            // TODO - add proxywms

            // Check if leeway is needed
            double leeway = run_one - wait_two;
            if (leeway > 0) {
                if (leeway > (run_two * 0.10)) {
                    // This much leeway is unaccpetable
                    continue;
                }
            } else {
                leeway = 0;
            }

            double total_time = wait_one + std::max<double>(run_one, wait_two) + run_two + leeway;

            // Only has to beat with beat_bound if the best grouping is still one_job-0
            double adjusted_time;
            if (partial_dag_end_level == end_level) {
                adjusted_time = total_time + (total_time * beat_bound);
            } else {
                adjusted_time = total_time;
            }

            if (adjusted_time < best_total_time) {
                // std::cout << "NEW TIME: " << total_time << std::endl;
                // std::cout << "ADJUSTED TIME: " << adjusted_time << std::endl;
                // std::cout << "BEST TOTAL: " << best_total_time << std::endl;
                partial_dag_end_level = (unsigned long) i;
                best_total_time = total_time;
                requested_execution_time = run_one;
                requested_parallelism = std::get<2>(start_to_split);
                estimated_wait_time = wait_one;
            }
        }

        // TODO add assert here
        if (partial_dag_end_level == end_level) {
            if (estimated_wait_time > requested_execution_time * 2.0) {
                // this->individual_mode = true;
                // std::cout << "INDIVIDUAL MODE" << std::endl;
            }
        } else {
            std::cout << "Splitting @ end level = " << partial_dag_end_level << std::endl;
            this->number_of_splits++;
        }

        if (not this->individual_mode) {
            // std::cerr << "Picking START LEVEL: " << start_level << " END LEVEL: " << partial_dag_end_level << " NODES: "
            std::cout << "Nodes: " << requested_parallelism << std::endl;
        }

        if (this->individual_mode) { WRENCH_INFO("GROUPING: INDIVIDUAL");
        } else { WRENCH_INFO("GROUPING: %ld-%ld",
                             start_level, partial_dag_end_level);
        }

//        std::cout << this->individual_mode << std::endl;
        if (not individual_mode) {
//            std::cout << "here " << start_level << " " << partial_dag_end_level << std::endl;
            // Add leeway
            if (parent_runtime > estimated_wait_time) {
//                std::cout << "ADDING LEEWAY: " << (parent_runtime - estimated_wait_time) << std::endl;
                requested_execution_time += parent_runtime - estimated_wait_time;
            }
            createAndSubmitPlaceholderJob(
                    requested_execution_time,
                    requested_parallelism,
                    start_level,
                    partial_dag_end_level);
        } else { WRENCH_INFO("Switching to individual mode!");
            // Submit all READY tasks as individual jobs
            for (auto task : this->getWorkflow()->getTasks()) {
                if (task->getState() == WorkflowTask::State::READY) {
                    StandardJob *standard_job = this->job_manager->createStandardJob(task, {});
                    std::map<std::string, std::string> service_specific_args;
                    requested_execution_time = (task->getFlops() / this->core_speed) * EXECUTION_TIME_FUDGE_FACTOR;
                    service_specific_args["-N"] = "1";
                    service_specific_args["-c"] = "1";
                    service_specific_args["-t"] = std::to_string(
                            1 + ((unsigned long) requested_execution_time) / 60);WRENCH_INFO(
                            "Submitting task %s individually!", task->getID().c_str());
                    this->job_manager->submitJob(standard_job, this->batch_service, service_specific_args);
                }
            }
        }

    }


    /**
    *
    * @param requested_execution_time
    * @param requested_parallelism
    * @param start_level
    * @param end_level
    */
    void TestClusteringWMS::createAndSubmitPlaceholderJob(
            double requested_execution_time,
            unsigned long requested_parallelism,
            unsigned long start_level,
            unsigned long end_level) {
//        std::cout << "REQUESTING: " << requested_execution_time << " " << requested_parallelism << " " << start_level
//                  << " " << end_level
//                  << std::endl;
        requested_execution_time = requested_execution_time * EXECUTION_TIME_FUDGE_FACTOR;
        parent_runtime = requested_execution_time;

        // Aggregate tasks
        std::vector<WorkflowTask *> tasks;
        for (unsigned long l = start_level; l <= end_level; l++) {
            std::vector<WorkflowTask *> tasks_in_level = this->getWorkflow()->getTasksInTopLevelRange(l, l);
            for (auto t : tasks_in_level) {
                if (t->getState() != WorkflowTask::COMPLETED) {
                    tasks.push_back(t);
                }
            }
        }

        // Submit the pilot job
        std::map<std::string, std::string> service_specific_args;
        service_specific_args["-N"] = std::to_string(requested_parallelism);
        service_specific_args["-c"] = "1";
        service_specific_args["-t"] = std::to_string(1 + ((unsigned long) requested_execution_time) / 60);

        WRENCH_INFO("Created a batch job with with batch arguments: %s:%s:%s",
                    service_specific_args["-N"].c_str(),
                    service_specific_args["-t"].c_str(),
                    service_specific_args["-c"].c_str());

        // Keep track of the placeholder job
        this->pending_placeholder_job = new TestPlaceHolderJob(
                this->job_manager->createPilotJob(),
                requested_parallelism,
                tasks,
                start_level,
                end_level);

        WRENCH_INFO("Submitting a Pilot Job (%ld hosts, %.2lf sec) for workflow levels %ld-%ld (%s)",
                    requested_parallelism, requested_execution_time, start_level, end_level,
                    this->pending_placeholder_job->pilot_job->getName().c_str());WRENCH_INFO(
                "This pilot job has these tasks:");
        for (auto t : this->pending_placeholder_job->tasks) { WRENCH_INFO("     - %s", t->getID().c_str());
        }

        // submit the corresponding pilot job
        this->job_manager->submitJob(this->pending_placeholder_job->pilot_job, this->batch_service,
                                     service_specific_args);
//        std::cout << "Finished submitting" << std::endl;
    }


    void TestClusteringWMS::processEventPilotJobStart(std::shared_ptr<PilotJobStartedEvent> e) {

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

        TestPlaceHolderJob *placeholder_job = this->pending_placeholder_job;

        // Move it to running
        this->running_placeholder_jobs.insert(placeholder_job);
        this->pending_placeholder_job = nullptr;

        // Submit all ready tasks to it each in its standard job, within node capacity
        std::string output_string = "";
        for (auto task : placeholder_job->tasks) { WRENCH_INFO("TASK %s:  READY=%d", task->getID().c_str(),
                                                               (task->getState() == WorkflowTask::READY));
            if ((task->getState() == WorkflowTask::READY) and
                (placeholder_job->num_currently_running_tasks < placeholder_job->num_nodes)) {
                StandardJob *standard_job = this->job_manager->createStandardJob(task, {});
                output_string += " " + task->getID();

                WRENCH_INFO("Submitting task %s as part of placeholder job %ld-%ld",
                            task->getID().c_str(), placeholder_job->start_level, placeholder_job->end_level);
                this->job_manager->submitJob(standard_job, placeholder_job->pilot_job->getComputeService());
                placeholder_job->num_currently_running_tasks++;WRENCH_INFO("NOW(2):  CURRENTLY  RUNNING: %lu",
                                                                           placeholder_job->num_currently_running_tasks);
            }
        }

        // Re-submit a pilot job so as to overlap execution of job n with waiting of job n+1
        this->applyGroupingHeuristic();

    }

    void TestClusteringWMS::processEventPilotJobExpiration(std::shared_ptr<PilotJobExpiredEvent> e) {
        std::cout << "GOT EXPIRATION" << std::endl;
        // Find the placeholder job
        TestPlaceHolderJob *placeholder_job = nullptr;
        for (auto ph : this->running_placeholder_jobs) {
            if (ph->pilot_job == e->pilot_job) {
                placeholder_job = ph;
                break;
            }
        }
        if (placeholder_job == nullptr) {
            throw std::runtime_error("Got a pilot job expiration, but no matching placeholder job found");
        }

        WRENCH_INFO("Got a pilot job expiration for a placeholder job that deals with levels %ld-%ld (%s)",
                    placeholder_job->start_level, placeholder_job->end_level,
                    placeholder_job->pilot_job->getName().c_str());

        this->running_placeholder_jobs.erase(placeholder_job);

        // Check if there are unprocessed tasks
        bool unprocessed = false;
        for (auto t : placeholder_job->tasks) {
            if (t->getState() != WorkflowTask::COMPLETED) {
                unprocessed = true;
                break;
            }
        }

//                        double wasted_node_seconds = e->pilot_job->getNumHosts() * e->pilot_job->getDuration();

        unsigned long num_hosts_used;
        sscanf(e->pilot_job->getServiceSpecificArguments()["-N"].c_str(), "%lu", &num_hosts_used);
        unsigned long minutes_used;
        sscanf(e->pilot_job->getServiceSpecificArguments()["-t"].c_str(), "%lu", &minutes_used);
        double wasted_node_seconds = 60.0 * minutes_used * num_hosts_used;

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

        std::set<TestPlaceHolderJob *> to_remove;
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

    void TestClusteringWMS::processEventStandardJobCompletion(std::shared_ptr<StandardJobCompletedEvent> e) {

        WorkflowTask *completed_task = e->standard_job->tasks[0]; // only one task per job

        WRENCH_INFO("Got a standard job completion for task %s", completed_task->getID().c_str());

        this->simulator->used_node_seconds += completed_task->getFlops() / this->core_speed;

        // Find the placeholder job this task belongs to
        TestPlaceHolderJob *placeholder_job = nullptr;
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

            placeholder_job->num_currently_running_tasks--;

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

        WRENCH_INFO("LOOKING FOR STUFF TO SCHEDULE!");
        // Start all newly ready tasks that depended on the completed task, IN ANY PLACEHOLDER
        // This shouldn't happen in individual mode, but can't hurt
//        std::vector<WorkflowTask *> children = this->getWorkflow()->getTaskChildren(completed_task);
        for (auto ph : this->running_placeholder_jobs) {
            for (auto task : ph->tasks) {
                if (
                        (task->getState() == WorkflowTask::READY) and
                        (ph->num_currently_running_tasks < ph->num_nodes)
                        ) {
                    StandardJob *standard_job = this->job_manager->createStandardJob(task, {});WRENCH_INFO(
                            "Submitting task %s  as part of placeholder job %ld-%ld",
                            task->getID().c_str(), ph->start_level, ph->end_level);
                    this->job_manager->submitJob(standard_job, ph->pilot_job->getComputeService());
                    ph->num_currently_running_tasks++;WRENCH_INFO("NOW: CURRETNLY RUNNING: %lu",
                                                                  ph->num_currently_running_tasks);
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

    void TestClusteringWMS::processEventStandardJobFailure(std::shared_ptr<StandardJobFailedEvent> e) {
        WRENCH_INFO("Got a standard job failure event for task %s -- IGNORING THIS",
                    e->standard_job->tasks[0]->getID().c_str());
    }

    double TestClusteringWMS::estimateWaitTime(long parallelism, double makespan, int *sequence) {
        std::set<std::tuple<std::string, unsigned int, unsigned int, double>> job_config;
        std::string config_key = "config_XXXX_" + std::to_string((*sequence)++); // need to make it unique for BATSCHED
        job_config.insert(std::make_tuple(config_key, (unsigned int) parallelism, 1, makespan));
        std::map<std::string, double> estimates = this->batch_service->getStartTimeEstimates(job_config);

        if (estimates[config_key] < 0) {
            throw std::runtime_error("Could not obtain start time estimate... aborting");
        }

        double wait_time_estimate = std::max<double>(0, estimates[config_key] -
                                                        this->simulation->getCurrentSimulatedDate());
        return wait_time_estimate;
    }

    /**
     * Minimize the total runtime by picking an optimal number of hosts
     * TODO rename to a more accurate name
     * @param start_level
     * @param end_level
     * @return (wait time, makespan, num_hosts)
     */
    //TODO: Take as input a % waste
    std::tuple<double, double, unsigned long> TestClusteringWMS::computeBestNumHosts(
            unsigned long start_level, unsigned long end_level) {
        double makespan = DBL_MAX;
        double wait_time = DBL_MAX;
        unsigned long num_hosts = 1;
        unsigned long max_tasks = TestClusteringWMS::findMaxTasks(start_level, end_level);
        for (unsigned long i = 1; i <= max_tasks; i++) {
            std::tuple<double, double> total_time = TestClusteringWMS::estimateTotalTime(start_level, end_level, i);
            double curr_makespan = std::get<0>(total_time);
            double curr_wait = std::get<1>(total_time);

            // Calculate the wasted ratio
            double all_tasks_time = 0;
            for (unsigned long j = start_level; j <= end_level; j++) {
                all_tasks_time += WorkflowUtil::estimateMakespan(
                        this->getWorkflow()->getTasksInTopLevelRange(j, j),
                        1, this->core_speed);
            }
            double curr_waste = (i * curr_makespan - all_tasks_time) / (i * curr_makespan);
            if (curr_waste > this->waste_bound) {
                continue;
            }

            if (makespan + wait_time > curr_makespan + curr_wait) {
                makespan = curr_makespan;
                wait_time = curr_wait;
                num_hosts = i;
            }
        }
        return std::make_tuple(wait_time, makespan, num_hosts);
    }

    /**
     * This returns a tuple of makespan and wait_time lol
     * @param start_level
     * @param end_level
     * @param num_hosts
     * @return (makespan, wait_time)
     */
    std::tuple<double, double> TestClusteringWMS::estimateTotalTime(
            unsigned long start_level, unsigned long end_level, unsigned long num_hosts) {
        static int sequence = 0;
        double makespan = WorkflowUtil::estimateMakespan(
                this->getWorkflow()->getTasksInTopLevelRange(start_level, end_level),
                num_hosts, this->core_speed);WRENCH_INFO("ESTIMATING WAIT TIME");
        double wait_time = TestClusteringWMS::estimateWaitTime(num_hosts, makespan, &sequence);WRENCH_INFO(
                "ESTIMATED WAIT TIME");
        return std::make_tuple(makespan, wait_time);
    }

    /**
     * Find the max tasks among levels in workflow group
     * @param start_level
     * @param end_level
     * @return
     */
    unsigned long TestClusteringWMS::findMaxTasks(unsigned long start_level, unsigned long end_level) {
        unsigned long max_tasks = 0;
        for (unsigned long i = start_level; i <= end_level; i++) {
            unsigned long num_tasks_in_level = this->getWorkflow()->getTasksInTopLevelRange(i, i).size();
            max_tasks = std::max<unsigned long>(max_tasks, num_tasks_in_level);
        }
        return max_tasks;
    }

};
