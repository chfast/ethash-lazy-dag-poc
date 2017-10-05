
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

struct cache_item
{
	std::array<uint64_t, 8> w = {{0, }};
};

cache_item create(size_t index)
{
	cache_item item;
	for (size_t i = 0; i < item.w.size(); ++i)
		item.w[i] = index + i + 17;
//	std::this_thread::sleep_for(std::chrono::microseconds(1));
	return item;
}

void validate(size_t index, const cache_item& item)
{
	for (size_t i = 0; i < item.w.size(); ++i)
	{
		if (item.w[i] != index + i + 17)
		{
			std::cerr << "Index: " << index << " flag: " << item.w[0] << "\n";
			throw short(1);
		}
	}
}

uint64_t hash(const cache_item& item)
{
	return std::accumulate(item.w.begin(), item.w.end(), uint64_t(0));
}

struct atomic_item
{
	std::array<std::atomic<uint64_t>, 8> a = {{{0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}}};

	cache_item lazy_load(size_t index)
	{
		cache_item item;

		uint64_t flag = a[0].load(std::memory_order_relaxed);
		if (flag == 0)
		{
			item = create(index);
			// Init item. Start from the end to set the first atomic
			// as the last write.
			for (auto i = a.rbegin(); i != a.rend(); ++i)
				i->store(item.w[7 - (i - a.rbegin())], std::memory_order_relaxed);
		}
		else
		{
			for (size_t i = 0; i < a.size(); ++i)
				item.w[i] = a[i].load(std::memory_order_relaxed);
		}

		return item;
	}
};

struct partial_atomic_item
{
	std::atomic<uint64_t> flag = {0};
	std::array<uint64_t, 7> data = {{0, }};

	cache_item lazy_load(size_t index)
	{
		uint64_t f = 0;
		if (flag.compare_exchange_strong(f, 1, std::memory_order_acq_rel))
		{
			auto item = create(index);

			for (size_t i = 0; i < data.size(); ++i)
				data[i] = item.w[i + 1];
			flag.store(item.w[0], std::memory_order_release);
			return item;
		}

		while (f == 1)
			f = flag.load(std::memory_order_acquire);

		cache_item item;
		item.w[0] = f;
		for (size_t i = 0; i < data.size(); ++i)
			item.w[i + 1] = data[i];
		return item;
	}
};

using thread_safe_item = partial_atomic_item;

static_assert(sizeof(thread_safe_item) == 8 * sizeof(uint64_t), "");


int main(int argc, const char* argv[])
{
	size_t t = 64;
	if (argc >= 2)
		t = std::stoul(argv[1]);

	size_t global_iterations = 100000000;
	if (argc >= 3)
		global_iterations = std::stoul(argv[2]);


	constexpr size_t cache_size = size_t(1) * 1024 * 1024 * 1024;
	constexpr size_t n = cache_size / sizeof(cache_item);
	size_t k = global_iterations / t;

	std::cout << "Cache size: " << cache_size << "\n";
	std::cout << "Cache items: " << n << "\n";
	std::cout << "Threads: " << t << "\n\n";
	std::cout << "Iterations : " << global_iterations << "\n";
	std::cout << "Iterations / thread : " << k << "\n";

	std::vector<thread_safe_item> dag(n);
	std::vector<std::thread> threads(t);

	auto start_time = std::chrono::high_resolution_clock::now();

	std::atomic<uint64_t> global_sum{0};
	for (auto& th: threads)
	{
		th = std::thread{[&dag, &global_sum, k]
		{
			uint64_t sum = 0;
			std::mt19937 gen(std::random_device{}());
			std::uniform_int_distribution<size_t> dis{0, n - 1};

			for (size_t j = 0; j < k; ++j)
			{
				size_t index = dis(gen);
				auto& it = dag[index];

				cache_item item = it.lazy_load(index);
				validate(index, item);
				sum += hash(item);
			}
			global_sum.fetch_add(sum);
		}};
	}

	for (auto&& th: threads)
		th.join();

	auto duration = std::chrono::high_resolution_clock::now() - start_time;
	auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
	auto access_rate = (1000 * t * k) / duration_ms;
	auto bandwidth = access_rate * sizeof(cache_item);

	std::cout << "SUM: " << global_sum << "\n";
	std::cout << "ACCESS RATE: " << (access_rate / 1000000.0) << " M/s\n";
	std::cout << "BANDWIDTH: " << (bandwidth / 1000000000.0) << " GB/s\n";

	return 0;
}