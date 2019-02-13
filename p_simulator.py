import subprocess
import time
import pymongo
import urllib.parse
from datetime import datetime
from threading import Thread
from threading import Lock
import time

lock = Lock()
commands = []

def simulator_command():
    executable = './build/simulator'
    num_compute_nodes = '100'
    job_trace_file = '../batch_logs/swf_traces_json/kth_sp2.json'
    max_sys_jobs = '1000'
    workflow_specification = 'levels:666:50:3600:3600:50:3600:3600:50:3600:3600:50:3600:3600'
    start_time = '100000'
    algorithm = 'evan:overlap:pnolimit'
    batch_algorithm = 'conservative_bf'
    log = '--wrench-no-log'
    return [executable, num_compute_nodes, job_trace_file, max_sys_jobs, workflow_specification, start_time, algorithm, batch_algorithm, log]

def simulation_dict(command):
    dict = {}
    dict['executable'] = command[0]
    dict['num_compute_nodes'] = command[1]
    dict['job_trace_file'] = command[2]
    dict['max_sys_jobs'] = command[3]
    dict['workflow_specification'] = command[4]
    dict['start_time'] = command[5]
    dict['algorithm'] = command[6]
    dict['batch_algorithm'] = command[7]
    dict['log'] = command[8]
    # Store run results!
    dict['success'] = False
    dict['runtime'] = -1
    dict['makespan'] = -1
    dict['num_p_job_exp'] = -1
    dict['total_queue_wait'] = -1
    dict['used_node_sec'] = -1
    dict['wasted_node_time'] = -1
    dict['error'] = ''
    # for zhang/evan
    # dict['split'] = False
    return dict

def print_command(command):
    print(" ".join(command))

def print_process_output(command, res, time):
    lock.acquire()
    print_command(command)
    res_arr = []
    for line in res.splitlines():
        line = line.decode("utf-8")
        print(line)
        res_arr.append(line)
    print("Simulation took %d seconds" % time)
    print("\n")
    lock.release()
    return res_arr

def write_to_mongo(obj):
    username = urllib.parse.quote_plus('evan')
    password = urllib.parse.quote_plus('password')
    myclient = pymongo.MongoClient('mongodb://%s:%s@dirt02.ics.hawaii.edu/simulations' % (username, password))
    mydb = myclient["simulations"]
    mycol = mydb["benchmark-3"]
    mycol.insert_one(obj)

def run_simulator(command):
    obj = simulation_dict(command)
    start = time.time()
    end = start
    try:
        # Timeout throws exception, this is okay i guess
        res = subprocess.check_output(command, timeout=900)
        end = time.time()
        res = print_process_output(command, res, end - start)
        obj['success'] = True
        obj['makespan'] = float((res[len(res) - 6]).split("=")[1])
        obj['num_p_job_exp'] = float((res[len(res) - 5]).split("=")[1])
        obj['total_queue_wait'] = float((res[len(res) - 4]).split("=")[1])
        obj['used_node_sec'] = float((res[len(res) - 3]).split("=")[1])
        obj['wasted_node_time'] = float((res[len(res) - 2]).split("=")[1])
        # for zhang/evan
        # if len(res) > 5:
        #     obj['split'] = True
    except Exception as e:
        lock.acquire()
        print_command(command)
        print('Exception in simulation: {}\n\n'.format(e))
        obj['success'] = False
        obj['error'] = str(e)
        lock.release()
    return
    obj['runtime'] = end - start
    obj['timestamp'] = datetime.utcnow()

    lock.acquire()
    try:
        # write_to_mongo(obj)
        pass
    except Exception as e:
        print("Mongo failure")
        print(obj)
        print('Exception in simulation: {}\n\n'.format(e))
    lock.release()

def execute():
    print("started thread")
    has_commands = True
    command = None
    while True:
        lock.acquire()
        if len(commands) == 0:
            command = None
        else:
            print("Running simulation: %d" % (len(commands)))
            command = commands.pop(0)
        lock.release()
        if command:
            run_simulator(command)
            # print_command(command)
        else:
            print("thread exiting")
            return

# TODO - will break if not a levels workflow
def get_algorithm(algorithm, workflow):
    if 'max' not in algorithm:
        return algorithm
    else:
        max_tasks = workflow[11:13]
        return 'static:one_job-' + max_tasks

def main():
    # trace_files = ['../batch_logs/swf_traces_json/kth_sp2.json']
    trace_files = ['../batch_logs/swf_traces_json/kth_sp2.json', '../batch_logs/swf_traces_json/sdsc_sp2.json']
    workflows = ['levels:666:10:3600:3600:10:3600:3600:10:3600:3600:10:3600:3600:10:3600:3600:10:3600:3600:10:3600:3600:10:3600:3600','levels:666:30:3600:3600:30:3600:3600:30:3600:3600:30:3600:3600:30:3600:3600:30:3600:3600:30:3600:3600:30:3600:3600', 'levels:666:50:3600:3600:50:3600:3600:50:3600:3600:50:3600:3600:50:3600:3600:50:3600:3600:50:3600:3600:50:3600:3600']
    start_times = ['125000', '156250', '195312', '244140', '305175', '381469', '476837', '596046', '745058', '931322']
    # Missing static:one_job-max
    algorithms = ['static:one_job-0', 'static:one_job-max', 'static:one_job_per_task', 'zhang:overlap:pnolimit', 'evan:overlap:pnolimit', 'test:overlap:pnolimit']
    # start_times = ['125000']
    # algorithms = ['static:one_job-0']
    # workflows = ['levels:666:10:3600:3600:10:3600:3600:10:3600:3600:10:3600:3600:10:3600:3600:10:3600:3600:10:3600:3600:10:3600:3600']

    for trace in trace_files:
        for workflow in workflows:
            for start_time in start_times:
                for algorithm in algorithms:
                    command = simulator_command()
                    if 'kth' in algorithm:
                        command[1] = '128'
                    else:
                        command[1] = '100'
                    command[2] = trace
                    command[4] = workflow
                    command[5] = start_time
                    command[6] = get_algorithm(algorithm, workflow)
                    commands.append(command)
    print("Total simulations to run: %d" % len(commands))
    '''
    for command in commands:
        print_command(command)
    print(len(commands))
    exit()
    '''
    threads = []
    cores = 8

    for i in range(cores):
        # if i > 0:
        #    time.sleep(15)
        thread = Thread(target=execute)
        thread.start()
        threads.append(thread)

    for thread in threads:
        thread.join()



if __name__ == '__main__':
    main()
