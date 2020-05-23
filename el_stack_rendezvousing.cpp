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

struct Node {
public:
	int key;
	Node * volatile next;

	Node() : next{ nullptr } {}
	Node(int key) : key{ key }, next{ nullptr } {}
	~Node() {}
};

bool CAS(Node* volatile * ptr, Node* old_value, Node* new_value) {
	return atomic_compare_exchange_strong(reinterpret_cast<volatile atomic_uintptr_t*>(ptr), reinterpret_cast<uintptr_t*>(&old_value), reinterpret_cast<uintptr_t>(new_value));
}

thread_local unsigned tid;
thread_local unsigned numa_id;

thread_local int exSize = 1; // thread 별로 교환자 크기를 따로 관리.
constexpr int MAX_PER_THREAD = 32;
//////////////////////////////////////////////////////////////////////
constexpr int WAITING_CNT = 1000;
constexpr int TRYING_CNT = 1000;
constexpr unsigned int INCREASE_THRESHOLD = MAX_PER_THREAD;
constexpr unsigned int DECREASE_THRESHOLD = 2;
constexpr unsigned int WAIT_THREASHOLD = WAITING_CNT;
//////////////////////////////////////////////////////////////////////

//static const unsigned NUM_NUMA_NODES = numa_num_configured_nodes();
//static const unsigned NUM_CPUS = numa_num_configured_cpus();

const unsigned NUM_NUMA_NODES = 4;
const unsigned NUM_CPUS = 64;

class Exchanger {
	volatile int value; // status와 교환값의 합성.

	enum Status { EMPTY, WAITING, DEPOSITED };
	bool CAS(int oldValue, int newValue, Status oldStatus, Status newStatus) {
		int oldV = oldValue << 2 | (int)oldStatus;
		int newV = newValue << 2 | (int)newStatus;
		return atomic_compare_exchange_strong(reinterpret_cast<atomic_int volatile *>(&value), &oldV, newV);
	}

public:
	bool capture() {
		if(Status(value & 0x3) == EMPTY){
			int tempVal = value >> 2;
			if(CAS(tempVal, 0, EMPTY, WAITING)){
				return true;
			}
		}
		return false;
	}

	int waiting(int& ctr) {
		for(ctr = 0; ctr < WAITING_CNT; ++ctr) {
			if (Status(value & 0x3) == DEPOSITED){
				int ret = value >> 2;
				value = EMPTY;
				return ret;
			}	
		}
		
		if(false == CAS(0, 0, WAITING, EMPTY)){
			int ret = value >> 2;
			value = EMPTY;
			return ret;
		}
		return -1;
	}

	bool deposit(int x){
		if(Status(value & 0x3) == WAITING){
			int temp = value >> 2;
			if (true == CAS(temp, x, WAITING, DEPOSITED)){
				return true;
			}
		}
		return false;
	}

	void init(){
		value = EMPTY; 
	}
};

class EliminationArray {
	Exchanger exchanger[MAX_PER_THREAD];

public:
	int findFreeNode(int s_idx, int& busy_ctr){
		busy_ctr = 0;
		while(true){
			if(exchanger[s_idx].capture()){
				return s_idx;
			}

			s_idx = (s_idx + 1) % exSize;
			++busy_ctr;
			if(busy_ctr > INCREASE_THRESHOLD){
				if (exSize < MAX_PER_THREAD - 1){
					++exSize;
				}
				busy_ctr = 0;
			}
		}
	}

	int get() {
		int s_idx = tid % exSize;	/////
		int busy_ctr = 0;
		int c_idx = findFreeNode(s_idx, busy_ctr);
		int ctr = 0;
		int ret = exchanger[c_idx].waiting(ctr);

		if(busy_ctr < DECREASE_THRESHOLD && ctr > WAITING_CNT && exSize > 1){
			--exSize;
		}

		return ret;	
	}

	bool put(int x) {
		int s_idx = tid % exSize;	/////
		int n_idx = (s_idx + 1) % exSize;

		for(int i = 0; i < TRYING_CNT; ++i) {
			if(exchanger[s_idx].deposit(x)){
				return true;
			}
			if(exchanger[n_idx].deposit(x)){
				return true;
			}
			n_idx = (s_idx + 1) % exSize;
		}
	}

	void init() {
		for(int i = 0; i < MAX_PER_THREAD; ++i){
			exchanger[i].init();
		}
	}
};


// Lock-Free Elimination BackOff Stack
class LFEBOStack {
	Node* volatile top;
	EliminationArray* eliminationArray[NUM_NUMA_NODES];
public:
	LFEBOStack() : top{ nullptr } {
        for(int i = 0; i < NUM_NUMA_NODES; ++i) {
            void *raw_ptr = numa_alloc_onnode(sizeof(EliminationArray), i);
            EliminationArray* ptr = new (raw_ptr) EliminationArray;
            eliminationArray[i] = ptr;
        }
    }
    ~LFEBOStack() {
        for (auto i = 0; i < NUM_NUMA_NODES; ++i)
        {
            eliminationArray[i]->~EliminationArray();
            numa_free(eliminationArray[i], sizeof(EliminationArray));
        }
    }

	void Push(int x) {
		auto e = new Node{ x };
		while (true)
		{
			bool result = eliminationArray[numa_id]->put(x);
			if (true == result) break;

			auto head = top;
			e->next = head;
			if (head != top) continue;
			if (true == CAS(&top, head, e)) return;
		}
	}

	int Pop() {
		while (true)
		{
			int result = eliminationArray[numa_id]->get();
			if(result != -1){
				return result;
			}
			auto head = top;
			if (nullptr == head) return 0;
			if (head != top) continue;
			if (true == CAS(&top, head, head->next)) return head->key;
		}
	}

	void clear() {
		for(int i = 0; i < NUM_NUMA_NODES; ++i) {
			eliminationArray[i]->init();
		}
		if (nullptr == top) return;
		while (top->next != nullptr) {
			Node *tmp = top;
			top = top->next;
			delete tmp;
		}
		delete top;
		top = nullptr;
	}

	void dump(size_t count) {
		auto& ptr = top;
		cout << count << " Result : ";
		for (auto i = 0; i < count; ++i) {
			if (nullptr == ptr) break;
			cout << ptr->key << ", ";
			ptr = ptr->next;
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