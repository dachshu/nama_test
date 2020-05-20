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


using namespace std;

#define NUM_THREAD 4

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

const int MAX_THREADS = 128;

const unsigned NUM_NUMA_NODES = 4;
const unsigned NUM_CPUS = 64;

enum OP{
	PUSH, POP, EMPTY
};

struct PROPER{
	atomic<OP> op {OP::EMPTY };
	atomic<int> val { -1 };
};

void helper_work(vector<PROPER*>* p_propers, stack<int>* p_seq_stack) {

		while (true)
		{
			for(int i = 0 ; i < NUM_THREAD; ++i){
				switch ((*p_propers)[i]->op.load(memory_order_acquire))
				{
				case OP::PUSH:{
					int val = (*p_propers)[i]->val.load(memory_order_acquire);
					(*p_propers)[i]->op.store(OP::EMPTY, memory_order_release);
					(*p_seq_stack).push(val);
					break;
				}
				case OP::POP:{
					if ((*p_seq_stack).empty()){
						(*p_propers)[i]->val.store(0, memory_order_release);
					}
					else{
						(*p_propers)[i]->val.store((*p_seq_stack).top(), memory_order_release);
					}
					
					(*p_propers)[i]->op.store(OP::EMPTY, memory_order_release);
					(*p_seq_stack).pop();

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
public:
    stack<int> seq_stack;
	thread helper;
    
    vector<PROPER*> propers;
public:
	DLStack() {
		propers.reserve(MAX_THREADS);
		unsigned num_core_per_node = NUM_CPUS / NUM_NUMA_NODES;
		for(int i = 0; i < MAX_THREADS; ++i) {
			PROPER* ptr = new PROPER;
			propers.emplace_back(ptr);
			//propers[i]  = ptr;
		}

		this->helper = thread{ helper_work, &propers, &seq_stack };
    }
    ~DLStack() {
        for (auto i = 0; i < MAX_THREADS; ++i)
        {	
			propers[i]->~PROPER();
			delete propers[i];
        }
		propers.clear();
    }

	
	void Push(int x) {
		propers[tid]->val.store(x, memory_order_release);
		propers[tid]->op.store(OP::PUSH, memory_order_release);
		while (propers[tid]->op.load(memory_order_acquire) != OP::EMPTY) { }
	}

	int Pop() {
		propers[tid]->op.store(OP::POP, memory_order_release);
		while (propers[tid]->op.load(memory_order_acquire) != OP::EMPTY) { }
		int ret =  propers[tid]->val.load(memory_order_acquire);
		return ret;
	}

	void clear() {
		for (auto i = 0; i < MAX_THREADS; ++i)
        {	
			propers[i]->val.store(-1);
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
			seq_stack.pop();
		}

		cout << "\n";
	}
} myStack;


void benchMark(int num_thread, int t) {
    tid = t;
    
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

	for (auto thread_num = NUM_THREAD; thread_num <= NUM_THREAD; thread_num *= 2) {
		//myStack.clear();
		threads.clear();

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
