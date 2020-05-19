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

class LFStack {
	Node* volatile top;
public:
	LFStack() : top{ nullptr } {}

	void Push(int x) {
		auto e = new Node{ x };
		while (true)
		{
			auto head = top;
			e->next = head;
			if (head != top) continue;
			if (true == CAS(&top, head, e)) return;
		}
	}

	int Pop() {
		while (true)
		{
			auto head = top;
			if (nullptr == head) return 0;
			if (head != top) continue;
			if (true == CAS(&top, head, head->next)) return head->key;
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