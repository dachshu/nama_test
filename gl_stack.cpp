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

thread_local int exSize = 1; // thread 별로 교환자 크기를 따로 관리.
constexpr int MAX_THREAD = 64;

class Exchanger {
	volatile int value; // status와 교환값의 합성.

	enum Status { EMPTY, WAIT, BUSY };
	bool CAS(int oldValue, int newValue, Status oldStatus, Status newStatus) {
		int oldV = oldValue << 2 | (int)oldStatus;
		int newV = newValue << 2 | (int)newStatus;
		return atomic_compare_exchange_strong(reinterpret_cast<atomic_int volatile *>(&value), &oldV, newV);
	}

public:
	int exchange(int x) {
		while (true) {
			switch (Status(value & 0x3)) {
			case EMPTY:
			{
				int tempVal = value >> 2;
				if (false == CAS(tempVal, x, EMPTY, WAIT)) continue;

				/* BUSY가 될 때까지 기다리며 timeout된 경우 -1 반환 */
				int count;
				for (count = 0; count < 100; ++count) {
					if (Status(value & 0x3) == BUSY) {
						int ret = value >> 2;
						value = EMPTY;
						return ret;
					}
				}
				if (false == CAS(tempVal, 0, WAIT, EMPTY)) { // 그 사이에 누가 들어온 경우
					int ret = value >> 2;
					value = EMPTY;
					return ret;
				}
				return -1;
			}
			break;
			case WAIT:
			{
				int temp = value >> 2;
				if (false == CAS(temp, x, WAIT, BUSY)) break;
				return temp;
			}
			break;
			case BUSY:
				if (exSize < MAX_THREAD) {
					exSize += 1;
				}
				return x;
			default:
				cerr <<  "It's impossible case\n" ;
				exit(1);
			}
		}
	}
};

class EliminationArray {
	Exchanger exchanger[MAX_THREAD];

public:
	int visit(int x) {
		int index = fast_rand() % exSize;
		return exchanger[index].exchange(x);
	}

	void shrink() {
		if (exSize > 1) exSize -= 1;
	}
};

// Lock-Free Elimination BackOff Stack
class LFEBOStack {
	Node* volatile top;
	EliminationArray eliminationArray;
public:
	LFEBOStack() : top{ nullptr } {}

	void Push(int x) {
		auto e = new Node{ x };
		while (true)
		{
			auto head = top;
			e->next = head;
			if (head != top) continue;
			if (true == CAS(&top, head, e)) return;
			int result = eliminationArray.visit(x);
			if (0 == result) break; // pop과 교환됨.
			if (-1 == result) eliminationArray.shrink(); // timeout 됨.
		}
	}

	int Pop() {
		while (true)
		{
			auto head = top;
			if (nullptr == head) return 0;
			if (head != top) continue;
			if (true == CAS(&top, head, head->next)) return head->key;
			int result = eliminationArray.visit(0);
			if (0 == result) continue; // pop끼리 교환되면 계속 시도
			if (-1 == result) eliminationArray.shrink(); // timeout 됨.
			else return result;
		}
	}

	void clear() {
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

void benchMark(int num_thread) {
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
		generate_n(back_inserter(threads), thread_num, [thread_num]() {return thread{ benchMark, thread_num }; });
		for (auto& t : threads) { t.join(); }
		auto du = chrono::high_resolution_clock::now() - start_t;

		myStack.dump(10);

		cout << thread_num << "Threads, Time = ";
		cout << chrono::duration_cast<chrono::milliseconds>(du).count() << "ms\n";
	}

}