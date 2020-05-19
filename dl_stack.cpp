#include <iostream>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>
#include <iterator>
#include <chrono>
#include <memory>
#include <stack>
#include <numa.h>

using namespace std;

static constexpr int NUM_TEST = 10000000;
static constexpr int RANGE = 1000;

unsigned long fast_rand(void)
{ //period 2^96-1
    static thread_local unsigned long x = 123456789, y = 362436069, z = 521288629;
    unsigned long t;
    x ^= x << 16;
    x ^= x >> 5;
    x ^= x << 1;

    t = x;
    x = y;
    y = z;
    z = t ^ x ^ y;

    return z;
}


thread_local unsigned tid;
thread_local unsigned numa_id;

int num_threads;
const int MAX_THREADS = 128;

const unsigned NUM_NUMA_NODES = 4;
const unsigned NUM_CPUS = 64;

enum OP{
	PUSH, POP, EMPTY
};

struct PROPER{
	atomic<OP> op {OP::EMPTY };
	int val { -1 };
};

void helper_work(vector<PROPER*>& propers, stack<int>& seq_stack) {
    	if( -1 == numa_run_on_node(0)){
        	cerr << "Error in pinning thread.. " << endl;
        	exit(1);
    	}
		while (true)
		{
			for(int i = 0 ; i < num_threads; ++i){
				switch (propers[i]->op.load(memory_order_acquire))
				{
				case OP::PUSH:{
					int val = propers[i]->val;
					propers[i]->op.store(OP::EMPTY, memory_order_release);
					seq_stack.push(val);
					break;
				}
				case OP::POP:{
					if (seq_stack.empty()){
						propers[i]->val = 0;
					}
					else{
						propers[i]->val = seq_stack.top();
					}
					
					propers[i]->op.store(OP::EMPTY, memory_order_release);
					seq_stack.pop();

					break;
				}
				default:
					break;
				}
			}
		}
		
	}


// Lock-Free Elimination BackOff Stack
class DLStack {
    stack<int> seq_stack;
	thread helper;
    
    vector<PROPER*> propers;
public:
	DLStack() {
		propers.reserve(MAX_THREADS);
		unsigned num_core_per_node = NUM_CPUS / NUM_NUMA_NODES;
		for(int i = 0; i < MAX_THREADS; ++i) {
			unsigned alloc_numa_id = (i / num_core_per_node) % NUM_NUMA_NODES;
			void *raw_ptr = numa_alloc_onnode(sizeof(PROPER), alloc_numa_id);
			PROPER* ptr = new (raw_ptr) PROPER;
			propers.emplace_back(ptr);
			//propers[i]  = ptr;
		}

		this->helper = thread{ helper_work, &propers, &seq_stack };
    }
    ~DLStack() {
        for (auto i = 0; i < MAX_THREADS; ++i)
        {	
			propers[i]->~PROPER();
			numa_free(propers[i], sizeof(PROPER));
        }
		propers.clear();
    }

	
	void Push(int x) {
		propers[tid]->val = x;
		propers[tid]->op.store(OP::PUSH, memory_order_release);
		while (propers[tid]->op.load(memory_order_acquire) != OP::EMPTY) { }
	}

	int Pop() {
		propers[tid]->op.store(OP::POP, memory_order_release);
		while (propers[tid]->op.load(memory_order_acquire) != OP::EMPTY) { }
		int ret =  propers[tid]->val;
		/////////////////////////////////////////
		if(ret == -1) {
			cout << "memory oreder error" << endl;
			while (true)
			{
			}			
		}
		////////////////////////////////////////////
		return ret;
	}

	void clear() {
		for (auto i = 0; i < MAX_THREADS; ++i)
        {	
			propers[i]->val = -1;
			propers[i]->op.store(OP::EMPTY);
        }
		while (seq_stack.empty() == false)
		{
			seq_stack.pop();
		}
	}

	void dump(size_t count) {
		cout << count << " Result : ";
		for (auto i = 0; i < count; ++i) {
			if (seq_stack.empty()) break;
			cout << seq_stack.top() << ", ";
		}
		cout << "\n";
	}
} myStack;


void benchMark(int num_thread, int t) {
    tid = t;
    unsigned num_core_per_node = NUM_CPUS / NUM_NUMA_NODES;
    numa_id = (tid / num_core_per_node) % NUM_NUMA_NODES;

    if( -1 == numa_run_on_node(numa_id)){
        cerr << "Error in pinning thread.. " << tid << ", " << numa_id << endl;
        exit(1);
    }
    
	for (int i = 1; i <= NUM_TEST / num_thread; ++i) {
		if ((fast_rand() % 2) || i <= 1000 / num_thread) {
			myStack.Push(i);
		}
		else {
			myStack.Pop();
		}
	}
}

int main() {

	vector<thread> threads;

	for (auto thread_num = 1; thread_num <= 128; thread_num *= 2) {
		myStack.clear();
		threads.clear();
		num_threads = thread_num;

		auto start_t = chrono::high_resolution_clock::now();
        for (int i = 0; i < thread_num; ++i)
            threads.push_back( thread{benchMark, thread_num, i} );
		//generate_n(back_inserter(threads), thread_num, [thread_num]() {return thread{ benchMark, thread_num }; });
		for (auto& t : threads) { t.join(); }
		auto du = chrono::high_resolution_clock::now() - start_t;

		myStack.dump(10);

		cout << thread_num << "Threads, Time = ";
		cout << chrono::duration_cast<chrono::milliseconds>(du).count() << "ms\n";
	}

}
