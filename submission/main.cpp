#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <thread>

using namespace std;

/* =========================================================
   Encode pair
========================================================= */
static inline uint64_t encode_pair(int i, int j)
{
    return (uint64_t(i) << 32) | (uint32_t)j;
}

/* =========================================================
   Parse TSV row
========================================================= */
vector<int> parse_row(
    const string &line,
    string &row_name,
    unordered_map<string, int> &col_map,
    vector<string> &col_names)
{
    size_t pos = line.find('\t');
    row_name = (pos == string::npos) ? line : line.substr(0, pos);

    vector<int> cols;
    size_t start = (pos == string::npos) ? line.size() : pos + 1;

    while (start < line.size())
    {
        size_t end = line.find('\t', start);
        string col = line.substr(start, end - start);

        auto it = col_map.find(col);
        int cid;

        if (it == col_map.end())
        {
            cid = (int)col_names.size();
            col_map[col] = cid;
            col_names.push_back(col);
        }
        else
        {
            cid = it->second;
        }

        cols.push_back(cid);

        if (end == string::npos)
            break;

        start = end + 1;
    }

    sort(cols.begin(), cols.end());
    cols.erase(unique(cols.begin(), cols.end()), cols.end());

    return cols;
}

/* =========================================================
   Build CSR-style inverted index (FLAT)
========================================================= */
void build_csr(
    const vector<vector<int>> &rows,
    vector<int> &offset,
    vector<int> &flat,
    int &max_col)
{
    max_col = 0;

    for (auto &r : rows)
        for (int c : r)
            max_col = max(max_col, c);

    vector<int> degree(max_col + 1, 0);

    for (auto &r : rows)
        for (int c : r)
            degree[c]++;

    offset.resize(max_col + 2);
    offset[0] = 0;

    for (int i = 0; i <= max_col; i++)
        offset[i + 1] = offset[i] + degree[i];

    flat.resize(offset[max_col + 1]);
    vector<int> cursor = offset;

    for (int i = 0; i < (int)rows.size(); i++)
    {
        for (int c : rows[i])
        {
            flat[cursor[c]++] = i;
        }
    }
}

/* =========================================================
   MULTITHREADED INTERSECTION COMPUTATION
========================================================= */
unordered_map<uint64_t, uint32_t>
compute_intersections_csr(
    const vector<int> &offset,
    const vector<int> &flat)
{
    int ncols = (int)offset.size() - 1;

    int hw = (int)thread::hardware_concurrency();
    if (hw <= 0) hw = 4;

    int threads = min(hw, max(1, ncols / 8));
    if (threads > ncols) threads = ncols;
    if (threads < 1) threads = 1;

    vector<unordered_map<uint64_t, uint32_t>> local_maps(threads);

    // pre-reserve for speed
    size_t approx = flat.size() / threads;
    for (auto &m : local_maps)
    {
        m.max_load_factor(0.7);
        m.reserve(approx);
    }

    auto worker = [&](int tid, int c_start, int c_end)
    {
        auto &inter = local_maps[tid];

        for (int c = c_start; c < c_end; c++)
        {
            int start = offset[c];
            int end = offset[c + 1];

            int f = end - start;
            if (f < 2) continue;

            for (int a = start; a < end; a++)
            {
                int i = flat[a];

                for (int b = a + 1; b < end; b++)
                {
                    int j = flat[b];

                    int x = i;
                    int y = j;
                    if (x > y)
                    {
                        int tmp = x;
                        x = y;
                        y = tmp;
                    }

                    inter[encode_pair(x, y)]++;
                }
            }
        }
    };

    vector<thread> pool;
    pool.reserve(threads);

    int chunk = (ncols + threads - 1) / threads;

    for (int t = 0; t < threads; t++)
    {
        int c_start = t * chunk;
        int c_end = min(ncols, (t + 1) * chunk);

        if (c_start >= c_end) continue;

        pool.emplace_back(worker, t, c_start, c_end);
    }

    for (auto &th : pool)
        th.join();

    // merge step (single-threaded but cheap relative to compute)
    unordered_map<uint64_t, uint32_t> inter;
    inter.max_load_factor(0.7);

    for (auto &m : local_maps)
    {
        for (auto &kv : m)
        {
            inter[kv.first] += kv.second;
        }
    }

    return inter;
}

/* =========================================================
   Output helpers (unchanged)
========================================================= */
void write_index(ofstream &out, const vector<string> &names)
{
    for (size_t i = 0; i < names.size(); i++)
        out << i << "\t" << names[i] << "\n";
}

void write_similarity(
    ofstream &out,
    const vector<vector<int>> &rows,
    const unordered_map<uint64_t, uint32_t> &inter)
{
    int N = (int)rows.size();

    out << fixed << setprecision(6);

    for (int i = 0; i < N; i++)
        if (!rows[i].empty())
            out << i << "\t" << i << "\t1.000000\n";

    for (auto &kv : inter)
    {
        uint64_t key = kv.first;
        uint32_t c = kv.second;

        int i = key >> 32;
        int j = key & 0xffffffff;

        double sim = (double)c /
            (rows[i].size() + rows[j].size() - c);

        out << i << "\t" << j << "\t" << sim << "\n";
    }
}

/* =========================================================
   MAIN
========================================================= */
int main(int argc, char **argv)
{
    if (argc != 5)
    {
        cerr << "Usage: " << argv[0]
             << " <input.tsv> <row.tsv> <col.tsv> <sim.tsv>\n";
        return 1;
    }

    ifstream in(argv[1]);
    ofstream rowf(argv[2]), colf(argv[3]), simf(argv[4]);

    if (!in || !rowf || !colf || !simf)
        return 1;

    unordered_map<string, int> col_map;
    vector<string> row_names, col_names;
    vector<vector<int>> rows;

    string line;

    while (getline(in, line))
    {
        string rn;
        rows.push_back(parse_row(line, rn, col_map, col_names));
        row_names.push_back(rn);
    }

    vector<int> offset, flat;
    int max_col;

    build_csr(rows, offset, flat, max_col);

    auto inter = compute_intersections_csr(offset, flat);

    write_index(rowf, row_names);
    write_index(colf, col_names);
    write_similarity(simf, rows, inter);

    return 0;
}