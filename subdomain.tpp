/*
 * Subdomain template file
 */

// Headers
#include <cstdarg>
#include <unistd.h>
#include <silo.h>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <algorithm>
#include "special_functions.hpp"
#include "timer.hpp"

// AMG functions
extern "C" void main_scaled_residual(Float*, Float*, const Float*, const Float*, const Float, const int, cudaStream_t);

void scaled_residual(amg::Vector &Sr, amg::Vector &w, amg::CSR_Matrix &A, const amg::Vector &u, const amg::Vector &f, const amg::Vector &S, const Float alpha, amg::Vector &work_dev)
{
    if (strcmp(A.mem_loc, "host") == 0)
    {
        for (int row = 0; row < A.num_rows; row++)
        {
            Float Ax = 0.0;

            for (int idx = A.ptr[row]; idx < A.ptr[row + 1]; idx++)
                Ax += A.val[idx] * u.data[A.col[idx]];

            Sr.data[row] = S.data[row] * (f.data[row] - Ax);
            w.data[row] = alpha * Sr.data[row];
        }
    }
    else
    {
        work_dev.copy_from(f);
        A.matvec(work_dev, u, - 1.0, 1.0);
        main_scaled_residual(Sr.data, w.data, work_dev.data, S.data, alpha, A.num_rows, work_dev.stream);
    }
}

extern "C" void vector_multiplication(Float*, const Float*, const Float*, const int, cudaStream_t);
extern "C" void main_polynomial_evaluation(Float*, Float*, const Float*, const Float*, const Float, const int, cudaStream_t);

void polynomial_evaluation(amg::Vector &w, amg::Vector &v, amg::CSR_Matrix &A, const amg::Vector &r, const amg::Vector &D_val, const Float alpha, amg::Vector &work_dev)
{
    if (strcmp(A.mem_loc, "host") == 0)
    {
        for (int row = 0; row < A.num_rows; row++)
        {
            Float tmp = 0.0;

            for (int idx = A.ptr[row]; idx < A.ptr[row + 1]; idx++)
                tmp += A.val[idx] * D_val.data[A.col[idx]] * w.data[A.col[idx]];

            v.data[row] = D_val.data[row] * tmp;
        }

        for (int row = 0; row < A.num_rows; row++)
            w.data[row] = alpha * r.data[row] + v.data[row];
    }
    else
    {
        vector_multiplication(work_dev.data, D_val.data, w.data, work_dev.size, work_dev.stream);
        A.matvec(v, work_dev, 1.0, 0.0);
        main_polynomial_evaluation(w.data, v.data, r.data, D_val.data, alpha, w.size, work_dev.stream);
    }
}

extern "C" void main_update_field(Float*, const Float*, const Float*, const int, cudaStream_t);

void update_field(amg::Vector &u, const amg::Vector &w, const amg::Vector &D_val)
{
    if (strcmp(u.mem_loc, "host") == 0)
    {
        for (int idx = 0; idx < u.size; idx++)
            u.data[idx] += D_val.data[idx] * w.data[idx];
    }
    else
    {
        main_update_field(u.data, w.data, D_val.data, u.size, u.stream);
    }
}

// Constructor and destructor
template<typename DType>
template<typename PType>
Subdomain<DType>::Subdomain(std::unordered_map<int, PType> domains, int poly_degree_, int poly_reduction_, int subdomain_overlap_, int superdomain_overlap_)
{
    // Fine level
    PType &domain = domains[poly_degree_];

    // Construct levels
    poly_reduction = poly_reduction_;
    subdomain_overlap = subdomain_overlap_;
    superdomain_overlap = superdomain_overlap_;

    poly_degree.push_back(poly_degree_);

    while (poly_degree[poly_degree.size() - 1] > 1)
    {
        int poly_degree_reduced = poly_degree[poly_degree.size() - 1] - poly_reduction;

        if (poly_degree_reduced >= 1)
            poly_degree.push_back(poly_degree_reduced);
        else
            poly_degree.push_back(1);
    }

    num_levels = poly_degree.size();

    levels.resize(num_levels);

    for (int l = 0; l < num_levels; l++)
    {
        levels[l].num_points = domains[poly_degree[l]].num_local_points;
        levels[l].num_elements = domains[poly_degree[l]].num_local_elements;
        levels[l].poly_degree = domains[poly_degree[l]].poly_degree;
        if (l > 0) levels[l].offset = levels[l - 1].offset + levels[l - 1].num_points;
    }

    // Work arrays
    int num_work_hst = dim;
    work_hst.resize(num_work_hst);

    int num_work_dev = dim;
    work_dev.resize(num_work_dev);

    // Prolongation/restriction reference operators
    std::vector<double> r_gll[num_levels];

    for (int l = 0; l < num_levels; l++)
    {
        int N_l = poly_degree[l];
        int n_l = N_l + 1;
        std::vector<double> w_gll(n_l);

        r_gll[l].resize(n_l);
        zwgll_(r_gll[l].data(), w_gll.data(), &n_l);
    }

    for (int l_f = 0; l_f < num_levels - 1; l_f++)
    {
        for (int l_c = l_f + 1; l_c < num_levels; l_c++)
        {
            int N_f = poly_degree[l_f];
            int N_c = poly_degree[l_c];
            int n_f = N_f + 1;
            int n_c = N_c + 1;
            std::pair<int, int> idx;

            // Coarse to fine
            idx.first = N_c;
            idx.second = N_f;
            J_cf[idx].first.resize(n_c * n_f);

            for (int i = 0; i < n_f; i++)
                for (int j = 1; j <= n_c; j++)
                    J_cf[idx].first[i * n_c + (j - 1)] = (DType)(hgll_(&j, &r_gll[l_f][i], r_gll[l_c].data(), &n_c));

            J_cf[idx].second = device.malloc<DType>(n_c * n_f);
            J_cf[idx].second.copyFrom(J_cf[idx].first.data(), n_c * n_f * sizeof(DType));
        }
    }

    // Operator
    std::vector<DType*> D_hat_ptr_hst(num_levels);
    D_hat.resize(num_levels);

    for (int l = 0; l < num_levels; l++)
    {
        int N_l = poly_degree[l];
        int n_l = N_l + 1;
        std::vector<double> D_gll(n_l * n_l);
        std::vector<double> Dt_gll(n_l * n_l);
        D_hat[l].first.resize(n_l * n_l);

        dgll_(Dt_gll.data(), D_gll.data(), r_gll[l].data(), &n_l, &n_l);
        for (int ij = 0; ij < n_l * n_l; ij++) D_hat[l].first[ij] = (DType)(D_gll[ij]);

        D_hat[l].second = device.malloc<DType>(n_l * n_l);
        D_hat[l].second.copyFrom(D_hat[l].first.data(), n_l * n_l * sizeof(DType));
        D_hat_ptr_hst[l] = (DType*)(D_hat[l].second.ptr());
    }

    D_hat_ptr = device.malloc<DType*>(num_levels * sizeof(DType*));
    D_hat_ptr.copyFrom(D_hat_ptr_hst.data(), num_levels * sizeof(DType*));

    for (int l = 0; l < num_levels; l++)
    {
        subdomain_operator.D_hat.push_back(D_hat[l].second);
        subdomain_operator.D_hat_ptr = D_hat_ptr;

        superdomain_operator.D_hat.push_back(D_hat[l].second);
        superdomain_operator.D_hat_ptr = D_hat_ptr;
    }

    // Gather geometric data of all elements (TODO: update to avoid doing this)
    int num_vertices = (dim == 2) ? 4 : 8;
    int num_edges = (dim == 2) ? 4 : 12;
    int num_faces = (dim == 2) ? 0 : 6;

    int num_total_elements = domain.num_total_elements;
    int num_local_elements = domain.num_local_elements;
    int num_total_points = num_total_elements * num_vertices;

    for (int d = 0; d < dim; d++)
    {
        int size = std::max({ num_total_elements, domain.num_local_points, num_levels });

        work_hst[d].resize((typeid(DType) == typeid(double)) ? size : 2 * size);
        work_dev[d].free();
        work_dev[d] = device.malloc<DType>((typeid(DType) == typeid(double)) ? size : 2 * size);
    }

    proc_count.resize(num_procs);
    proc_offset.resize(num_procs);

    proc_count[proc_id] = num_local_elements * num_vertices;
    MPI_Allgather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, proc_count.data(), 1, MPI_INT, MPI_COMM_WORLD);

    proc_offset[0] = 0;
    for (int p = 1; p < num_procs; p++) proc_offset[p] = proc_offset[p - 1] + proc_count[p - 1];

    std::vector<long long> geometry_mesh(num_total_points);

    if (dim == 2)
    {
        int n_x = domain.poly_degree + 1;
        int n_y = n_x;

        for (auto &elem : domain.elements)
        {
            geometry_mesh[proc_offset[proc_id] + (elem.id * num_vertices + 0)] = elem.glo_num[0 + 0 * n_x];
            geometry_mesh[proc_offset[proc_id] + (elem.id * num_vertices + 1)] = elem.glo_num[(n_x - 1) + 0 * n_x];
            geometry_mesh[proc_offset[proc_id] + (elem.id * num_vertices + 2)] = elem.glo_num[0 + (n_y - 1) * n_x];
            geometry_mesh[proc_offset[proc_id] + (elem.id * num_vertices + 3)] = elem.glo_num[(n_x - 1) + (n_y - 1) * n_x];
        }

        MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, geometry_mesh.data(), proc_count.data(), proc_offset.data(), MPI_LONG_LONG, MPI_COMM_WORLD);
    }
    else
    {
        int n_x = domain.poly_degree + 1;
        int n_y = n_x;
        int n_z = n_x;
        int n_xy = n_x * n_y;

        for (auto &elem : domain.elements)
        {
            geometry_mesh[proc_offset[proc_id] + (elem.id * num_vertices + 0)] = elem.glo_num[0 + 0 * n_x + 0 * n_xy];
            geometry_mesh[proc_offset[proc_id] + (elem.id * num_vertices + 1)] = elem.glo_num[(n_x - 1) + 0 * n_x + 0 * n_xy];
            geometry_mesh[proc_offset[proc_id] + (elem.id * num_vertices + 2)] = elem.glo_num[0 + (n_y - 1) * n_x + 0 * n_xy];
            geometry_mesh[proc_offset[proc_id] + (elem.id * num_vertices + 3)] = elem.glo_num[(n_x - 1) + (n_y - 1) * n_x + 0 * n_xy];
            geometry_mesh[proc_offset[proc_id] + (elem.id * num_vertices + 4)] = elem.glo_num[0 + 0 * n_x + (n_z - 1) * n_xy];
            geometry_mesh[proc_offset[proc_id] + (elem.id * num_vertices + 5)] = elem.glo_num[(n_x - 1) + 0 * n_x + (n_z - 1) * n_xy];
            geometry_mesh[proc_offset[proc_id] + (elem.id * num_vertices + 6)] = elem.glo_num[0 + (n_y - 1) * n_x + (n_z - 1) * n_xy];
            geometry_mesh[proc_offset[proc_id] + (elem.id * num_vertices + 7)] = elem.glo_num[(n_x - 1) + (n_y - 1) * n_x + (n_z - 1) * n_xy];
        }

        MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, geometry_mesh.data(), proc_count.data(), proc_offset.data(), MPI_LONG_LONG, MPI_COMM_WORLD);
    }

    for (int p = 0; p < num_procs; p++)
    {
        proc_count[p] /= num_vertices;
        proc_offset[p] /= num_vertices;
    }

    // Element to processor partition
    std::vector<std::pair<int, int>> partition(num_total_elements);

    for (int p = 0; p < num_procs; p++)
    {
        for (int e = 0; e < proc_count[p]; e++)
        {
            partition[proc_offset[p] + e].first = p;
            partition[proc_offset[p] + e].second = e;
        }
    }

    // Mesh connectivity
    std::vector<std::vector<std::set<int>>> vert_conn(num_total_elements, std::vector<std::set<int>>(num_vertices));
    std::vector<std::vector<std::set<int>>> edge_conn(num_total_elements, std::vector<std::set<int>>(num_edges));
    std::vector<std::vector<std::set<int>>> face_conn(num_total_elements, std::vector<std::set<int>>(num_faces));

    CSR_Matrix<DType> expander;

    if (dim >= 1) // Vertices
    {
        std::map<long long, std::set<int>> vertices;

        for (int e = 0; e < num_total_elements; e++)
            for (int vid = 0; vid < num_vertices; vid++)
                vertices[geometry_mesh[e * num_vertices + vid]].insert(e);

        for (int e = 0; e < num_total_elements; e++)
        {
            for (int vid = 0; vid < num_vertices; vid++)
            {
                std::set<int> &neighbors = vertices[geometry_mesh[e * num_vertices + vid]];
                vert_conn[e][vid].insert(neighbors.begin(), neighbors.end());
                vert_conn[e][vid].erase(e);
            }
        }
    }

    if (dim == 2) // Edges
    {
        std::map<std::tuple<long long, long long>, std::set<int>> edges;
        std::vector<long long> edge(2);
        std::vector<std::tuple<int, int>> edge_pairs
        {
            std::tuple<int, int>(0, 1), 
            std::tuple<int, int>(2, 3), 
            std::tuple<int, int>(0, 2), 
            std::tuple<int, int>(1, 3)
        };

        for (int e = 0; e < num_total_elements; e++)
        {
            for (auto edge_pair : edge_pairs)
            {
                edge[0] = geometry_mesh[e * num_vertices + std::get<0>(edge_pair)];
                edge[1] = geometry_mesh[e * num_vertices + std::get<1>(edge_pair)];
                std::sort(edge.begin(), edge.end());
                edges[std::tuple<long long, long long>(edge[0], edge[1])].insert(e);
            }
        }

        for (int e = 0; e < num_total_elements; e++)
        {
            for (int eid = 0; eid < num_edges; eid++)
            {
                edge[0] = geometry_mesh[e * num_vertices + std::get<0>(edge_pairs[eid])];
                edge[1] = geometry_mesh[e * num_vertices + std::get<1>(edge_pairs[eid])];
                std::sort(edge.begin(), edge.end());
                std::set<int> &neighbors = edges[std::tuple<long long, long long>(edge[0], edge[1])];
                edge_conn[e][eid].insert(neighbors.begin(), neighbors.end());
                edge_conn[e][eid].erase(e);
            }
        }
    }
    else
    {
        std::map<std::tuple<long long, long long>, std::set<int>> edges;
        std::vector<long long> edge(2);
        std::vector<std::tuple<int, int>> edge_pairs
        {
            std::tuple<int, int>(0, 1), 
            std::tuple<int, int>(2, 3), 
            std::tuple<int, int>(0, 2), 
            std::tuple<int, int>(1, 3), 
            std::tuple<int, int>(4, 5), 
            std::tuple<int, int>(6, 7), 
            std::tuple<int, int>(4, 6), 
            std::tuple<int, int>(5, 7), 
            std::tuple<int, int>(0, 4), 
            std::tuple<int, int>(1, 5), 
            std::tuple<int, int>(2, 6), 
            std::tuple<int, int>(3, 7), 
        };

        for (int e = 0; e < num_total_elements; e++)
        {
            for (auto edge_pair : edge_pairs)
            {
                edge[0] = geometry_mesh[e * num_vertices + std::get<0>(edge_pair)];
                edge[1] = geometry_mesh[e * num_vertices + std::get<1>(edge_pair)];
                std::sort(edge.begin(), edge.end());
                edges[std::tuple<long long, long long>(edge[0], edge[1])].insert(e);
            }
        }

        for (int e = 0; e < num_total_elements; e++)
        {
            for (int eid = 0; eid < num_edges; eid++)
            {
                edge[0] = geometry_mesh[e * num_vertices + std::get<0>(edge_pairs[eid])];
                edge[1] = geometry_mesh[e * num_vertices + std::get<1>(edge_pairs[eid])];
                std::sort(edge.begin(), edge.end());
                std::set<int> &neighbors = edges[std::tuple<long long, long long>(edge[0], edge[1])];
                edge_conn[e][eid].insert(neighbors.begin(), neighbors.end());
                edge_conn[e][eid].erase(e);
            }
        }
    }

    if (dim == 3) // Faces
    {
        std::map<std::tuple<long long, long long, long long, long long>, std::set<int>> faces;
        std::vector<long long> face(4);
        std::vector<std::tuple<long long, long long, long long, long long>> face_pairs
        {
            std::tuple<long long, long long, long long, long long>(0, 1, 2, 3), 
            std::tuple<long long, long long, long long, long long>(4, 5, 6, 7), 
            std::tuple<long long, long long, long long, long long>(0, 1, 4, 5), 
            std::tuple<long long, long long, long long, long long>(2, 3, 6, 7), 
            std::tuple<long long, long long, long long, long long>(0, 2, 4, 6), 
            std::tuple<long long, long long, long long, long long>(1, 3, 5, 7), 
        };

        for (int e = 0; e < num_total_elements; e++)
        {
            for (auto face_pair : face_pairs)
            {
                face[0] = geometry_mesh[e * num_vertices + std::get<0>(face_pair)];
                face[1] = geometry_mesh[e * num_vertices + std::get<1>(face_pair)];
                face[2] = geometry_mesh[e * num_vertices + std::get<2>(face_pair)];
                face[3] = geometry_mesh[e * num_vertices + std::get<3>(face_pair)];
                std::sort(face.begin(), face.end());
                faces[std::tuple<long long, long long, long long, long long>(face[0], face[1], face[2], face[3])].insert(e);
            }
        }

        for (int e = 0; e < num_total_elements; e++)
        {
            for (int fid = 0; fid < num_faces; fid++)
            {
                face[0] = geometry_mesh[e * num_vertices + std::get<0>(face_pairs[fid])];
                face[1] = geometry_mesh[e * num_vertices + std::get<1>(face_pairs[fid])];
                face[2] = geometry_mesh[e * num_vertices + std::get<2>(face_pairs[fid])];
                face[3] = geometry_mesh[e * num_vertices + std::get<3>(face_pairs[fid])];
                std::sort(face.begin(), face.end());
                std::set<int> &neighbors = faces[std::tuple<long long, long long, long long, long long>(face[0], face[1], face[2], face[3])];
                face_conn[e][fid].insert(neighbors.begin(), neighbors.end());
                face_conn[e][fid].erase(e);
            }
        }
    }

    // Matrix construction
    expander.initialize(num_total_elements, num_total_elements);

    for (int e_i = 0; e_i < num_total_elements; e_i++)
    {
        expander.add_entry(e_i, e_i, 1.0);

        for (auto data : vert_conn[e_i])
            for (auto e_j : data)
                expander.add_entry(e_i, e_j, 1.0);

        for (auto data : edge_conn[e_i])
            for (auto e_j : data)
                expander.add_entry(e_i, e_j, 1.0);

        for (auto data : face_conn[e_i])
            for (auto e_j : data)
                expander.add_entry(e_i, e_j, 1.0);
    }

    expander.assemble();
    math.set_to_value(expander.val, 1.0, expander.num_nnz);

    // Construct computational regions
    std::vector<Element<DType>> subdomain_region;
    std::vector<Element<DType>> superdomain_region;

    std::vector<int> subdomain_partition(num_total_elements);
    std::vector<int> superdomain_partition(num_total_elements);

    num_subdomain_elems = 0;
    num_superdomain_elems = 0;

    num_subdomain_extended_elems = 0;
    num_superdomain_extended_elems = 0;

    for (int e = 0; e < num_local_elements; e++)
    {
        subdomain_region.push_back(Element<DType>(proc_offset[proc_id] + e, dim, poly_degree[0]));
        subdomain_partition[proc_offset[proc_id] + e] = num_subdomain_elems + 1;
        num_subdomain_elems++;
        num_subdomain_extended_elems++;
    }

    work_hst[0].assign(num_total_elements, 0.0);
    work_hst[1].assign(num_total_elements, 0.0);

    for (int e = 0; e < num_local_elements; e++)
    {
        work_hst[0][proc_offset[proc_id] + e] = 1.0;
        work_hst[1][proc_offset[proc_id] + e] = (DType)(e + 1);
    }

    work_dev[0].copyFrom(work_hst[0].data(), num_total_elements * sizeof(DType));

    for (int l = 0; l < num_levels; l++)
    {
        for (int nu = 0; nu < subdomain_overlap; nu++)
        {
            expander.multiply(work_dev[1], work_dev[0]);
            work_dev[0].copyFrom(work_dev[1], num_total_elements * sizeof(DType));
        }
    
        work_dev[0].copyTo(work_hst[0].data(), num_total_elements * sizeof(DType));

        for (int e = 0; e < num_total_elements; e++)
        {
            if ((work_hst[0][e] > 0.0) and (work_hst[1][e] == 0.0))
            {
                work_hst[1][e] = (DType)(num_subdomain_elems);
                subdomain_region.push_back(Element<DType>(e, dim, poly_degree[l]));
                subdomain_partition[e] = num_subdomain_elems + 1;
                num_subdomain_elems++;
                num_subdomain_extended_elems++;
            }
        }

        if (subdomain_overlap == 0) subdomain_overlap = 1;
    }

    expander.multiply(work_dev[1], work_dev[0]);
    work_dev[1].copyTo(work_hst[0].data(), num_total_elements * sizeof(DType));

    for (int e = 0; e < num_total_elements; e++)
    {
        if (work_hst[1][e] == 0.0)
        {
            if (work_hst[0][e] > 0.0)
            {
                subdomain_region.push_back(Element<DType>(e, dim, poly_degree[num_levels - 1]));
                subdomain_partition[e] = num_subdomain_extended_elems + 1;
                num_subdomain_extended_elems++;
            }

            superdomain_region.push_back(Element<DType>(e, dim, poly_degree[num_levels - 1]));
            superdomain_partition[e] = num_superdomain_elems + 1;
            num_superdomain_elems++;
            num_superdomain_extended_elems++;
        }
    }

    for (int e = 0; e < num_total_elements; e++)
    {
        if (work_hst[1][e] == 0.0)
            work_hst[0][e] = 1.0;
        else
            work_hst[0][e] = 0.0;
    }

    work_dev[0].copyFrom(work_hst[0].data(), num_total_elements * sizeof(DType));
    expander.multiply(work_dev[1], work_dev[0]);
    work_dev[1].copyTo(work_hst[0].data(), num_total_elements * sizeof(DType));

    for (int e = 0; e < num_total_elements; e++)
    {
        if ((work_hst[0][e] > 0.0) and (work_hst[1][e] > 0.0))
        {
            superdomain_region.push_back(Element<DType>(e, dim, poly_degree[num_levels - 1]));
            superdomain_partition[e] = num_superdomain_extended_elems + 1;
            num_superdomain_extended_elems++;
        }
    }

    num_subdomain_points = 0;
    num_subdomain_extended_points = 0;

    for (int e = 0; e < num_subdomain_extended_elems; e++)
    {
        auto &elem = subdomain_region[e];

        if (num_subdomain_extended_points > 0) elem.offset += num_subdomain_extended_points;
        if (e < num_subdomain_elems) num_subdomain_points += elem.num_points;

        num_subdomain_extended_points += elem.num_points;
    }

    num_superdomain_points = 0;
    num_superdomain_extended_points = 0;

    for (int e = 0; e < num_superdomain_extended_elems; e++)
    {
        auto &elem = superdomain_region[e];

        if (num_superdomain_extended_points > 0) elem.offset += num_superdomain_extended_points;
        if (e < num_superdomain_elems) num_superdomain_points += elem.num_points;

        num_superdomain_extended_points += elem.num_points;
    }

    // Coarsening tree
    std::unordered_map<int, int> total_points_offset(num_levels);
    total_points_offset[poly_degree[0]] = 0;

    for (int l = 1; l < num_levels; l++)
        total_points_offset[poly_degree[l]] += total_points_offset[poly_degree[l - 1]] + num_total_elements * std::pow(poly_degree[l - 1] + 1, dim);

    for (int d = 0; d < dim; d++)
    {
        int size = std::max({ (int)(work_hst[d].size()), total_points_offset[poly_degree[num_levels - 1]] + num_total_elements * (int)(std::pow(poly_degree[num_levels - 1] + 1, dim)) + num_subdomain_extended_points + num_superdomain_extended_points });

        work_hst[d].resize((typeid(DType) == typeid(double)) ? size : 2 * size);
        work_dev[d].free();
        work_dev[d] = device.malloc<DType>((typeid(DType) == typeid(double)) ? size : 2 * size);
    }

    for (int w = 0; w < num_work_dev; w++) ((DType**)(work_hst[0].data()))[w] = (DType*)(work_dev[w].ptr());
    work_dev_ptr = device.malloc<DType*>(num_work_dev * sizeof(DType*));
    work_dev_ptr.copyFrom(work_hst[0].data(), num_work_dev * sizeof(DType*));

    int loc_off = 0;

    for (int l = 0; l < num_levels; l++)
    {
        int num_points = std::pow(poly_degree[l] + 1, dim);
        int glo_off = total_points_offset[poly_degree[l]] + proc_offset[proc_id] * num_points;

        for (int e = 0; e < num_local_elements; e++)
        {
            for (int v = 0; v < num_points; v++)
                ((long long*)(work_hst[0].data()))[loc_off++] = glo_off + v + 1;

            glo_off += num_points;
        }
    }

    subdomain_offset = loc_off;

    for (auto &elem : subdomain_region)
    {
        int glo_off = proc_offset[partition[elem.id].first] * elem.num_points;
        glo_off += partition[elem.id].second * elem.num_points;
        glo_off += total_points_offset[elem.poly_degree];

        for (int v = 0; v < elem.num_points; v++)
            ((long long*)(work_hst[0].data()))[loc_off++] = - (glo_off + v + 1);
    }

    superdomain_offset = loc_off;

    for (auto &elem : superdomain_region)
    {
        int glo_off = proc_offset[partition[elem.id].first] * elem.num_points;
        glo_off += partition[elem.id].second * elem.num_points;
        glo_off += total_points_offset[elem.poly_degree];

        for (int v = 0; v < elem.num_points; v++)
            ((long long*)(work_hst[0].data()))[loc_off++] = - (glo_off + v + 1);
    }

    comm_init(&gs_comm, MPI_COMM_WORLD);
    gs_handle = gslib_gs_setup((long long*)(work_hst[0].data()), loc_off, &gs_comm, 0, gs_auto, 1);

    // Computational regions setup
    int level_offset = 0;

    for (int l = 0; l < num_levels; l++)
    {
        for (auto &dlem : domains[poly_degree[l]].elements)
            for (int v = 0; v < dlem.num_points; v++)
                work_hst[0][level_offset + dlem.offset + v] = dlem.dirichlet_mask[v];

        level_offset += num_local_elements * (int)(std::pow(poly_degree[l] + 1, dim));
    }

    memset(work_hst[0].data() + subdomain_offset, 0, (num_subdomain_extended_points + num_superdomain_extended_points) * sizeof(DType));
    gslib_gs(work_hst[0].data(), gs_type, gs_add, 0, gs_handle, NULL);

    for (auto &elem : subdomain_region)
        for (int v = 0; v < elem.num_points; v++)
            elem.dirichlet_mask[v] = work_hst[0][subdomain_offset + elem.offset + v];

    for (auto &elem : superdomain_region)
        for (int v = 0; v < elem.num_points; v++)
            elem.dirichlet_mask[v] = work_hst[0][superdomain_offset + elem.offset + v];

    for (int g = 0; g < NUM_GEOM_FACTS; g++)
    {
        level_offset = 0;

        for (int l = 0; l < num_levels; l++)
        {
            for (auto &dlem : domains[poly_degree[l]].elements)
                for (int v = 0; v < dlem.num_points; v++)
                    work_hst[0][level_offset + dlem.offset + v] = dlem.geom_fact[g][v];

            level_offset += num_local_elements * (int)(std::pow(poly_degree[l] + 1, dim));
        }

        memset(work_hst[0].data() + subdomain_offset, 0, (num_subdomain_extended_points + num_superdomain_extended_points) * sizeof(DType));
        gslib_gs(work_hst[0].data(), gs_type, gs_add, 0, gs_handle, NULL);

        subdomain_operator.geom_fact[g] = device.malloc<DType>(num_subdomain_extended_points);
        subdomain_operator.geom_fact[g].copyFrom(work_hst[0].data() + subdomain_offset, num_subdomain_extended_points * sizeof(DType));

        for (int g = 0; g < NUM_GEOM_FACTS; g++) ((DType**)(work_hst[0].data()))[g] = (DType*)(subdomain_operator.geom_fact[g].ptr());
        subdomain_operator.geom_fact_ptr = device.malloc<DType*>(NUM_GEOM_FACTS * sizeof(DType*));
        subdomain_operator.geom_fact_ptr.copyFrom(work_hst[0].data(), NUM_GEOM_FACTS * sizeof(DType*));

        if (num_superdomain_extended_points > 0)
        {
            superdomain_operator.geom_fact[g] = device.malloc<DType>(num_superdomain_extended_points);
            superdomain_operator.geom_fact[g].copyFrom(work_hst[0].data() + superdomain_offset, num_superdomain_extended_points * sizeof(DType));

            for (int g = 0; g < NUM_GEOM_FACTS; g++) ((DType**)(work_hst[0].data()))[g] = (DType*)(superdomain_operator.geom_fact[g].ptr());
            superdomain_operator.geom_fact_ptr = device.malloc<DType*>(NUM_GEOM_FACTS * sizeof(DType*));
            superdomain_operator.geom_fact_ptr.copyFrom(work_hst[0].data(), NUM_GEOM_FACTS * sizeof(DType*));
        }
    }

    level_offset = 0;

    for (int l = 0; l < num_levels; l++)
    {
        for (auto &dlem : domains[poly_degree[l]].elements)
            for (int v = 0; v < dlem.num_points; v++)
                work_hst[0][level_offset + dlem.offset + v] = (DType)(dlem.glo_num[v]);

        level_offset += num_local_elements * (int)(std::pow(poly_degree[l] + 1, dim));
    }

    memset(work_hst[0].data() + subdomain_offset, 0, (num_subdomain_extended_points + num_superdomain_extended_points) * sizeof(DType));
    gslib_gs(work_hst[0].data(), gs_type, gs_add, 0, gs_handle, NULL);

    for (auto &elem : subdomain_region)
        for (int v = 0; v < elem.num_points; v++)
            elem.glo_num[v] = (long long)(work_hst[0][subdomain_offset + elem.offset + v]);

    for (auto &elem : superdomain_region)
        for (int v = 0; v < elem.num_points; v++)
            elem.glo_num[v] = (long long)(work_hst[0][superdomain_offset + elem.offset + v]);

    for (auto &elem : subdomain_region)
        for (int v = 0; v < elem.num_points; v++)
            elem.loc_num[v] = elem.offset + v;

    for (auto &elem : superdomain_region)
        for (int v = 0; v < elem.num_points; v++)
            elem.loc_num[v] = elem.offset + v;

    // Geometry of elements
    if (dim >= 1)
    {
        level_offset = 0;

        for (int l = 0; l < num_levels; l++)
        {
            for (auto &dlem : domains[poly_degree[l]].elements)
                for (int v = 0; v < dlem.num_points; v++)
                    work_hst[0][level_offset + dlem.offset + v] = (DType)(dlem.x[v]);

            level_offset += num_local_elements * (int)(std::pow(poly_degree[l] + 1, dim));
        }

        memset(work_hst[0].data() + subdomain_offset, 0, (num_subdomain_extended_points + num_superdomain_extended_points) * sizeof(DType));
        gslib_gs(work_hst[0].data(), gs_type, gs_add, 0, gs_handle, NULL);

        for (auto &elem : subdomain_region)
            for (int v = 0; v < elem.num_points; v++)
                elem.x[v] = work_hst[0][subdomain_offset + elem.offset + v];

        for (auto &elem : superdomain_region)
            for (int v = 0; v < elem.num_points; v++)
                elem.x[v] = work_hst[0][superdomain_offset + elem.offset + v];
    }

    if (dim >= 2)
    {
        level_offset = 0;

        for (int l = 0; l < num_levels; l++)
        {
            for (auto &dlem : domains[poly_degree[l]].elements)
                for (int v = 0; v < dlem.num_points; v++)
                    work_hst[0][level_offset + dlem.offset + v] = (DType)(dlem.y[v]);

            level_offset += num_local_elements * (int)(std::pow(poly_degree[l] + 1, dim));
        }

        memset(work_hst[0].data() + subdomain_offset, 0, (num_subdomain_extended_points + num_superdomain_extended_points) * sizeof(DType));
        gslib_gs(work_hst[0].data(), gs_type, gs_add, 0, gs_handle, NULL);

        for (auto &elem : subdomain_region)
            for (int v = 0; v < elem.num_points; v++)
                elem.y[v] = work_hst[0][subdomain_offset + elem.offset + v];

        for (auto &elem : superdomain_region)
            for (int v = 0; v < elem.num_points; v++)
                elem.y[v] = work_hst[0][superdomain_offset + elem.offset + v];
    }

    if (dim >= 3)
    {
        level_offset = 0;

        for (int l = 0; l < num_levels; l++)
        {
            for (auto &dlem : domains[poly_degree[l]].elements)
                for (int v = 0; v < dlem.num_points; v++)
                    work_hst[0][level_offset + dlem.offset + v] = (DType)(dlem.z[v]);

            level_offset += num_local_elements * (int)(std::pow(poly_degree[l] + 1, dim));
        }

        memset(work_hst[0].data() + subdomain_offset, 0, (num_subdomain_extended_points + num_superdomain_extended_points) * sizeof(DType));
        gslib_gs(work_hst[0].data(), gs_type, gs_add, 0, gs_handle, NULL);

        for (auto &elem : subdomain_region)
            for (int v = 0; v < elem.num_points; v++)
                elem.z[v] = work_hst[0][subdomain_offset + elem.offset + v];

        for (auto &elem : superdomain_region)
            for (int v = 0; v < elem.num_points; v++)
                elem.z[v] = work_hst[0][superdomain_offset + elem.offset + v];
    }

    for (int e = 0; e < num_subdomain_elems; e++) elements.push_back(subdomain_region[e]);
    for (int e = 0; e < num_superdomain_elems; e++) elements.push_back(superdomain_region[e]);

    // Interface nodes
    std::unordered_set<long long> subdomain_glo_num;
    std::unordered_set<long long> interface_glo_num;

    for (int e = 0; e < num_subdomain_elems; e++)
    {
        auto &elem = subdomain_region[e];

        if (elem.poly_degree == 1)
            for (int v = 0; v < elem.num_points; v++)
                if (elem.dirichlet_mask[v] > 0.0)
                    subdomain_glo_num.insert(elem.glo_num[v]);
    }

    for (int e = 0; e < num_superdomain_elems; e++)
    {
        auto &elem = superdomain_region[e];

        for (int v = 0; v < elem.num_points; v++)
            if (subdomain_glo_num.find(elem.glo_num[v]) != subdomain_glo_num.end())
                interface_glo_num.insert(elem.glo_num[v]);
    }

    subdomain_glo_num.clear();

    for (auto &elem : subdomain_region)
        for (int v = 0; v < elem.num_points; v++)
            if (interface_glo_num.find(elem.glo_num[v]) != interface_glo_num.end())
                elem.dof_num[v] = elem.glo_num[v];

    for (auto &elem : superdomain_region)
        for (int v = 0; v < elem.num_points; v++)
            if (interface_glo_num.find(elem.glo_num[v]) != interface_glo_num.end())
                elem.dof_num[v] = elem.glo_num[v];

    // Connectivity of regions (TODO: still based on global data stored locally)
    std::vector<int> subdomain_mapping(num_total_elements);
    std::vector<int> superdomain_mapping(num_total_elements);

    for (auto &region : { std::pair<std::vector<Element<DType>>&, std::vector<int>&>(subdomain_region, subdomain_mapping),
                          std::pair<std::vector<Element<DType>>&, std::vector<int>&>(superdomain_region, superdomain_mapping) })
    {
        auto &elements = region.first;
        auto &mapping = region.second;

        for (unsigned int e = 0; e < elements.size(); e++)
            mapping[elements[e].id] = e + 1;

        for (auto &elem : elements)
        {
            // Vertices
            for (int vid = 0; vid < num_vertices; vid++)
                for (auto e_j : vert_conn[elem.id][vid])
                    if (mapping[e_j] > 0)
                        elem.vert_conn[vid].insert(mapping[e_j] - 1);

            // Edges
            for (int eid = 0; eid < num_edges; eid++)
                for (auto e_j : edge_conn[elem.id][eid])
                    if (mapping[e_j] > 0)
                        elem.edge_conn[eid].insert(mapping[e_j] - 1);

            // Faces
            for (int fid = 0; fid < num_faces; fid++)
                for (auto e_j : face_conn[elem.id][fid])
                    if (mapping[e_j] > 0)
                        elem.face_conn[fid].insert(mapping[e_j] - 1);
        }
    }

    // Ranking function
    auto ranking = [&](std::vector<DType> &data, int size)
    {
        if (size == 0) return;

        std::vector<std::pair<unsigned int, DType>> entries(data.size());

        for (int i = 0; i < size; i++)
        {
            entries[i].first = i;
            entries[i].second = data[i];
        }

        std::sort(entries.begin(), entries.begin() + size, [&](const std::pair<unsigned int, DType> a, const std::pair<unsigned int, DType> b){ return a.second < b.second; });

        DType value = entries[0].second;
        DType rank = (value == 0.0) ? 0.0 : 1.0;

        entries[0].second = rank;

        for (int i = 1; i < size; i++)
        {
            auto &entry = entries[i];

            if (entry.second == value)
            {
                entry.second = rank;
            }
            else
            {
                rank += 1.0;
                value = entry.second;
                entry.second = rank;
            }
        }

        std::sort(entries.begin(), entries.begin() + size);
        for (int i = 0; i < size; i++) data[i] = entries[i].second;
    };

    // Global numbering (need to zero out non-conforming edges
    std::unordered_map<int, long long> global_offset;
    global_offset[poly_degree[0]] = 0;
    for (int l = 1; l < num_levels; l++)
        global_offset[poly_degree[l]] = global_offset[poly_degree[l - 1]] + (long long)(domain.num_total_elements) * (long long)(std::pow(poly_degree[l - 1] + 1, dim));

    if (dim == 2)
    {
        for (auto &elem : subdomain_region)
        {
            std::vector<long long> corners({ elem.glo_num[             0 +              0 * elem.n_x], 
                                             elem.glo_num[(elem.n_x - 1) +              0 * elem.n_x], 
                                             elem.glo_num[             0 + (elem.n_y - 1) * elem.n_x], 
                                             elem.glo_num[(elem.n_x - 1) + (elem.n_y - 1) * elem.n_x]});

            for (int v = 0; v < elem.num_points; v++) elem.glo_num[v] += global_offset[elem.poly_degree];

            elem.glo_num[             0 +              0 * elem.n_x] = corners[0];
            elem.glo_num[(elem.n_x - 1) +              0 * elem.n_x] = corners[1];
            elem.glo_num[             0 + (elem.n_y - 1) * elem.n_x] = corners[2];
            elem.glo_num[(elem.n_x - 1) + (elem.n_y - 1) * elem.n_x] = corners[3];
        }
    }
    else
    {
        for (auto &elem : subdomain_region)
        {
            std::vector<long long> corners({ elem.glo_num[             0 +              0 * elem.n_x +              0 * elem.n_x * elem.n_y], 
                                             elem.glo_num[(elem.n_x - 1) +              0 * elem.n_x +              0 * elem.n_x * elem.n_y], 
                                             elem.glo_num[             0 + (elem.n_y - 1) * elem.n_x +              0 * elem.n_x * elem.n_y], 
                                             elem.glo_num[(elem.n_x - 1) + (elem.n_y - 1) * elem.n_x +              0 * elem.n_x * elem.n_y], 
                                             elem.glo_num[             0 +              0 * elem.n_x + (elem.n_z - 1) * elem.n_x * elem.n_y], 
                                             elem.glo_num[(elem.n_x - 1) +              0 * elem.n_x + (elem.n_z - 1) * elem.n_x * elem.n_y], 
                                             elem.glo_num[             0 + (elem.n_y - 1) * elem.n_x + (elem.n_z - 1) * elem.n_x * elem.n_y], 
                                             elem.glo_num[(elem.n_x - 1) + (elem.n_y - 1) * elem.n_x + (elem.n_z - 1) * elem.n_x * elem.n_y]});

            for (int v = 0; v < elem.num_points; v++) elem.glo_num[v] += global_offset[elem.poly_degree];

            elem.glo_num[             0 +              0 * elem.n_x +              0 * elem.n_x * elem.n_y] = corners[0];
            elem.glo_num[(elem.n_x - 1) +              0 * elem.n_x +              0 * elem.n_x * elem.n_y] = corners[1];
            elem.glo_num[             0 + (elem.n_y - 1) * elem.n_x +              0 * elem.n_x * elem.n_y] = corners[2];
            elem.glo_num[(elem.n_x - 1) + (elem.n_y - 1) * elem.n_x +              0 * elem.n_x * elem.n_y] = corners[3];
            elem.glo_num[             0 +              0 * elem.n_x + (elem.n_z - 1) * elem.n_x * elem.n_y] = corners[4];
            elem.glo_num[(elem.n_x - 1) +              0 * elem.n_x + (elem.n_z - 1) * elem.n_x * elem.n_y] = corners[5];
            elem.glo_num[             0 + (elem.n_y - 1) * elem.n_x + (elem.n_z - 1) * elem.n_x * elem.n_y] = corners[6];
            elem.glo_num[(elem.n_x - 1) + (elem.n_y - 1) * elem.n_x + (elem.n_z - 1) * elem.n_x * elem.n_y] = corners[7];
        }
    }

    if (dim == 2)
    {
        for (auto &elem_i : subdomain_region)
        {
            int n_x_i = elem_i.n_x;
            int n_y_i = elem_i.n_y;

            for (int eid = 0; eid < num_edges; eid++)
            {
                for (auto e_j : elem_i.edge_conn[eid])
                {
                    auto &elem_j = subdomain_region[e_j];

                    if (elem_j.poly_degree < elem_i.poly_degree)
                    {
                        if (eid == 0)
                            for (int i = 1; i < n_x_i - 1; i++) elem_i.glo_num[i + 0 * n_x_i] = 0;

                        else if (eid == 1)
                            for (int i = 1; i < n_x_i - 1; i++) elem_i.glo_num[i + (n_y_i - 1) * n_x_i] = 0;

                        else if (eid == 2)
                            for (int j = 1; j < n_y_i - 1; j++) elem_i.glo_num[0 + j * n_x_i] = 0;

                        else if (eid == 3)
                            for (int j = 1; j < n_y_i - 1; j++) elem_i.glo_num[(n_x_i - 1) + j * n_x_i] = 0;
                    }
                }
            }
        }
    }
    else
    {
        for (auto &elem_i : subdomain_region)
        {
            int n_x_i = elem_i.n_x;
            int n_y_i = elem_i.n_y;
            int n_z_i = elem_i.n_z;

            int n_xy_i = n_x_i * n_y_i;

            for (int eid = 0; eid < num_edges; eid++)
            {
                for (auto e_j : elem_i.edge_conn[eid])
                {
                    auto &elem_j = subdomain_region[e_j];

                    if (elem_j.poly_degree < elem_i.poly_degree)
                    {
                        if (eid == 0)
                            for (int i = 1; i < n_x_i - 1; i++) elem_i.glo_num[i + 0 * n_x_i + 0 * n_xy_i] = 0;

                        else if (eid == 1)
                            for (int i = 1; i < n_x_i - 1; i++) elem_i.glo_num[i + (n_y_i - 1) * n_x_i + 0 * n_xy_i] = 0;

                        else if (eid == 2)
                            for (int j = 1; j < n_y_i - 1; j++) elem_i.glo_num[0 + j * n_x_i + 0 * n_xy_i] = 0;

                        else if (eid == 3)
                            for (int j = 1; j < n_y_i - 1; j++) elem_i.glo_num[(n_x_i - 1) + j * n_x_i + 0 * n_xy_i] = 0;

                        else if (eid == 4)
                            for (int i = 1; i < n_x_i - 1; i++) elem_i.glo_num[i + 0 * n_x_i + (n_z_i - 1) * n_xy_i] = 0;

                        else if (eid == 5)
                            for (int i = 1; i < n_x_i - 1; i++) elem_i.glo_num[i + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i] = 0;

                        else if (eid == 6)
                            for (int j = 1; j < n_y_i - 1; j++) elem_i.glo_num[0 + j * n_x_i + (n_z_i - 1) * n_xy_i] = 0;

                        else if (eid == 7)
                            for (int j = 1; j < n_y_i - 1; j++) elem_i.glo_num[(n_x_i - 1) + j * n_x_i + (n_z_i - 1) * n_xy_i] = 0;

                        else if (eid == 8)
                            for (int k = 1; k < n_z_i - 1; k++) elem_i.glo_num[0 + 0 * n_x_i + k * n_xy_i] = 0;

                        else if (eid == 9)
                            for (int k = 1; k < n_z_i - 1; k++) elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i + k * n_xy_i] = 0;

                        else if (eid == 10)
                            for (int k = 1; k < n_z_i - 1; k++) elem_i.glo_num[0 + (n_y_i - 1) * n_x_i + k * n_xy_i] = 0;

                        else if (eid == 11)
                            for (int k = 1; k < n_z_i - 1; k++) elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i + k * n_xy_i] = 0;
                    }
                }
            }

            for (int fid = 0; fid < num_faces; fid++)
            {
                for (auto e_j : elem_i.face_conn[fid])
                {
                    auto &elem_j = subdomain_region[e_j];

                    if (elem_j.poly_degree < elem_i.poly_degree)
                    {
                        if (fid == 0)
                            for (int j = 1; j < n_y_i - 1; j++)
                                for (int i = 1; i < n_x_i - 1; i++)
                                    elem_i.glo_num[i + j * n_x_i + 0 * n_xy_i] = 0;

                        else if (fid == 1)
                            for (int j = 1; j < n_y_i - 1; j++)
                                for (int i = 1; i < n_x_i - 1; i++)
                                    elem_i.glo_num[i + j * n_x_i + (n_z_i - 1) * n_xy_i] = 0;

                        else if (fid == 2)
                            for (int k = 1; k < n_z_i - 1; k++)
                                for (int i = 1; i < n_x_i - 1; i++)
                                    elem_i.glo_num[i + 0 * n_x_i + k * n_xy_i] = 0;

                        else if (fid == 3)
                            for (int k = 1; k < n_z_i - 1; k++)
                                for (int i = 1; i < n_x_i - 1; i++)
                                    elem_i.glo_num[i + (n_y_i - 1) * n_x_i + k * n_xy_i] = 0;

                        else if (fid == 4)
                            for (int k = 1; k < n_z_i - 1; k++)
                                for (int j = 1; j < n_y_i - 1; j++)
                                    elem_i.glo_num[0 + j * n_x_i + k * n_xy_i] = 0;

                        else if (fid == 5)
                            for (int k = 1; k < n_z_i - 1; k++)
                                for (int j = 1; j < n_y_i - 1; j++)
                                    elem_i.glo_num[(n_x_i - 1) + j * n_x_i + k * n_xy_i] = 0;
                    }
                }
            }
        }
    }

    // Mark interface nodes second to last and extended nodes last in the subdomain region
    long long max_subdomain_num = 0;

    for (auto &elem : subdomain_region)
        for (auto glo : elem.glo_num)
            max_subdomain_num = std::max(max_subdomain_num, glo);

    for (auto &elem : subdomain_region)
        if (elem.poly_degree == 1)
            for (int v = 0; v < elem.num_points; v++)
                if (elem.dof_num[v] > 0)
                    elem.glo_num[v] += max_subdomain_num;

    for (auto &elem : subdomain_region)
        for (auto glo : elem.glo_num)
            max_subdomain_num = std::max(max_subdomain_num, glo);

    for (int e = num_subdomain_elems; e < (int)(subdomain_region.size()); e++)
    {
        auto &elem = subdomain_region[e];

        for (int v = 0; v < elem.num_points; v++)
            if ((elem.dirichlet_mask[v] > 0.0) and (elem.dof_num[v] == 0))
                elem.glo_num[v] += max_subdomain_num;
    }

    // Mark interface nodes first, regular nodes second, and then extended nodes last in the superdomain region
    long long max_superdomain_num = 0;

    for (auto &elem : superdomain_region)
        for (auto glo : elem.glo_num)
            max_superdomain_num = std::max(max_superdomain_num, glo);

    for (auto &elem : superdomain_region)
        for (int v = 0; v < elem.num_points; v++)
            if ((elem.dirichlet_mask[v] > 0.0) and (elem.dof_num[v] == 0))
                elem.glo_num[v] += max_superdomain_num;

    for (auto &elem : superdomain_region)
        for (auto glo : elem.glo_num)
            max_superdomain_num = std::max(max_superdomain_num, glo);

    for (int e = num_superdomain_elems; e < (int)(superdomain_region.size()); e++)
    {
        auto &elem = superdomain_region[e];

        for (int v = 0; v < elem.num_points; v++)
            if ((elem.dirichlet_mask[v] > 0.0) and (elem.dof_num[v] == 0))
                elem.glo_num[v] += max_superdomain_num;
    }

    for (auto &region : { std::pair<std::vector<Element<DType>>&, int>(subdomain_region, num_subdomain_extended_points), 
                          std::pair<std::vector<Element<DType>>&, int>(superdomain_region, num_superdomain_extended_points) })
    {
        auto &elements = region.first;
        auto &num_points = region.second;

        for (auto &elem : elements)
            for (int v = 0; v < elem.num_points; v++)
                work_hst[0][elem.offset + v] = (DType)(elem.glo_num[v]);

        ranking(work_hst[0], num_points);

        for (auto &elem : elements)
            for (int v = 0; v < elem.num_points; v++)
                elem.glo_num[v] = (long long)(work_hst[0][elem.offset + v]);

        for (auto &elem : elements)
            for (int v = 0; v < elem.num_points; v++)
                work_hst[0][elem.offset + v] = (DType)(elem.glo_num[v]) * elem.dirichlet_mask[v];

        ranking(work_hst[0], num_points);

        for (auto &elem : elements)
            for (int v = 0; v < elem.num_points; v++)
                elem.dof_num[v] = (long long)(work_hst[0][elem.offset + v]);
    }

    // Region operator setup
    auto matching_edge = [&](Element<DType> elem_i, Element<DType> elem_j, int eid)
    {
        int n_x_i = elem_i.n_x;
        int n_y_i = elem_i.n_y;
        int n_z_i = elem_i.n_z;
        int n_xy_i = n_x_i * n_y_i;

        int n_x_j = elem_j.n_x;
        int n_y_j = elem_j.n_y;
        int n_z_j = elem_j.n_z;
        int n_xy_j = n_x_j * n_y_j;

        std::set<long long> edge;
        std::vector<int> idx_i(n_x_i);
        std::vector<int> idx_j(n_x_j);

        if (dim == 2)
        {
            if (eid == 0)
            {
                edge.insert(elem_i.glo_num[0 + 0 * n_x_i]);
                edge.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i]);
                for (int k = 0; k < n_x_i; k++) idx_i[k] = k + 0 * n_x_i;
            }
            else if (eid == 1)
            {
                edge.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i]);
                edge.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i]);
                for (int k = 0; k < n_x_i; k++) idx_i[k] = k + (n_y_i - 1) * n_x_i;
            }
            else if (eid == 2)
            {
                edge.insert(elem_i.glo_num[0 + 0 * n_x_i]);
                edge.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i]);
                for (int k = 0; k < n_y_i; k++) idx_i[k] = 0 + k * n_x_i;

            }
            else if (eid == 3)
            {
                edge.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i]);
                edge.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i]);
                for (int k = 0; k < n_y_i; k++) idx_i[k] = (n_x_i - 1) + k * n_x_i;
            }

            if ((edge.find(elem_j.glo_num[0 + 0 * n_x_j]) != edge.end()) and (edge.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j]) != edge.end()))
                for (int k = 0; k < n_x_j; k++) idx_j[k] = k + 0 * n_x_j;

            else if ((edge.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j]) != edge.end()) and (edge.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j]) != edge.end()))
                for (int k = 0; k < n_x_j; k++) idx_j[k] = k + (n_y_j - 1) * n_x_j;

            else if ((edge.find(elem_j.glo_num[0 + 0 * n_x_j]) != edge.end()) and (edge.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j]) != edge.end()))
                for (int k = 0; k < n_y_j; k++) idx_j[k] = 0 + k * n_x_j;

            else if ((edge.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j]) != edge.end()) and (edge.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j]) != edge.end()))
                for (int k = 0; k < n_y_j; k++) idx_j[k] = (n_x_j - 1) + k * n_x_j;
        }
        else
        {
            if (eid == 0)
            {
                edge.insert(elem_i.glo_num[0 + 0 * n_x_i + 0 * n_xy_i]);
                edge.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i + 0 * n_xy_i]);
                for (int k = 0; k < n_x_i; k++) idx_i[k] = k + 0 * n_x_i + 0 * n_xy_i;
            }
            else if (eid == 1)
            {
                edge.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i + 0 * n_xy_i]);
                edge.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i + 0 * n_xy_i]);
                for (int k = 0; k < n_x_i; k++) idx_i[k] = k + (n_y_i - 1) * n_x_i + 0 * n_xy_i;
            }
            else if (eid == 2)
            {
                edge.insert(elem_i.glo_num[0 + 0 * n_x_i + 0 * n_xy_i]);
                edge.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i + 0 * n_xy_i]);
                for (int k = 0; k < n_y_i; k++) idx_i[k] = 0 + k * n_x_i + 0 * n_xy_i;
            }
            else if (eid == 3)
            {
                edge.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i + 0 * n_xy_i]);
                edge.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i + 0 * n_xy_i]);
                for (int k = 0; k < n_y_i; k++) idx_i[k] = (n_x_i - 1) + k * n_x_i + 0 * n_xy_i;
            }
            else if (eid == 4)
            {
                edge.insert(elem_i.glo_num[0 + 0 * n_x_i + (n_z_i - 1) * n_xy_i]);
                edge.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i + (n_z_i - 1) * n_xy_i]);
                for (int k = 0; k < n_x_i; k++) idx_i[k] = k + 0 * n_x_i + (n_z_i - 1) * n_xy_i;
            }
            else if (eid == 5)
            {
                edge.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i]);
                edge.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i]);
                for (int k = 0; k < n_x_i; k++) idx_i[k] = k + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i;
            }
            else if (eid == 6)
            {
                edge.insert(elem_i.glo_num[0 + 0 * n_x_i + (n_z_i - 1) * n_xy_i]);
                edge.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i]);
                for (int k = 0; k < n_y_i; k++) idx_i[k] = 0 + k * n_x_i + (n_z_i - 1) * n_xy_i;
            }
            else if (eid == 7)
            {
                edge.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i + (n_z_i - 1) * n_xy_i]);
                edge.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i]);
                for (int k = 0; k < n_y_i; k++) idx_i[k] = (n_x_i - 1) + k * n_x_i + (n_z_i - 1) * n_xy_i;
            }
            else if (eid == 8)
            {
                edge.insert(elem_i.glo_num[0 + 0 * n_x_i + 0 * n_xy_i]);
                edge.insert(elem_i.glo_num[0 + 0 * n_x_i + (n_z_i - 1) * n_xy_i]);
                for (int k = 0; k < n_z_i; k++) idx_i[k] = 0 + 0 * n_x_i + k * n_xy_i;
            }
            else if (eid == 9)
            {
                edge.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i + 0 * n_xy_i]);
                edge.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i + (n_z_i - 1) * n_xy_i]);
                for (int k = 0; k < n_z_i; k++) idx_i[k] = (n_x_i - 1) + 0 * n_x_i + k * n_xy_i;
            }
            else if (eid == 10)
            {
                edge.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i + 0 * n_xy_i]);
                edge.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i]);
                for (int k = 0; k < n_z_i; k++) idx_i[k] = 0 + (n_y_i - 1) * n_x_i + k * n_xy_i;
            }
            else if (eid == 11)
            {
                edge.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i + 0 * n_xy_i]);
                edge.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i]);
                for (int k = 0; k < n_z_i; k++) idx_i[k] = (n_x_i - 1) + (n_y_i - 1) * n_x_i + k * n_xy_i;
            }

            if ((edge.find(elem_j.glo_num[0 + 0 * n_x_j + 0 * n_xy_j]) != edge.end()) and (edge.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j + 0 * n_xy_j]) != edge.end()))
                for (int k = 0; k < n_x_j; k++) idx_j[k] = k + 0 * n_x_j + 0 * n_xy_j;

            else if ((edge.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j + 0 * n_xy_j]) != edge.end()) and (edge.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j + 0 * n_xy_j]) != edge.end()))
                for (int k = 0; k < n_x_j; k++) idx_j[k] = k + (n_y_j - 1) * n_x_j + 0 * n_xy_j;

            else if ((edge.find(elem_j.glo_num[0 + 0 * n_x_j + 0 * n_xy_j]) != edge.end()) and (edge.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j + 0 * n_xy_j]) != edge.end()))
                for (int k = 0; k < n_y_j; k++) idx_j[k] = 0 + k * n_x_j + 0 * n_xy_j;

            else if ((edge.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j + 0 * n_xy_j]) != edge.end()) and (edge.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j + 0 * n_xy_j]) != edge.end()))
                for (int k = 0; k < n_y_j; k++) idx_j[k] = (n_x_j - 1) + k * n_x_j + 0 * n_xy_j;

            else if ((edge.find(elem_j.glo_num[0 + 0 * n_x_j + (n_z_j - 1) * n_xy_j]) != edge.end()) and (edge.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j + (n_z_j - 1) * n_xy_j]) != edge.end()))
                for (int k = 0; k < n_x_j; k++) idx_j[k] = k + 0 * n_x_j + (n_z_j - 1) * n_xy_j;

            else if ((edge.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j + (n_z_j - 1) * n_xy_j]) != edge.end()) and (edge.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j + (n_z_j - 1) * n_xy_j]) != edge.end()))
                for (int k = 0; k < n_x_j; k++) idx_j[k] = k + (n_y_j - 1) * n_x_j + (n_z_j - 1) * n_xy_j;

            else if ((edge.find(elem_j.glo_num[0 + 0 * n_x_j + (n_z_j - 1) * n_xy_j]) != edge.end()) and (edge.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j + (n_z_j - 1) * n_xy_j]) != edge.end()))
                for (int k = 0; k < n_y_j; k++) idx_j[k] = 0 + k * n_x_j + (n_z_j - 1) * n_xy_j;

            else if ((edge.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j + (n_z_j - 1) * n_xy_j]) != edge.end()) and (edge.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j + (n_z_j - 1) * n_xy_j]) != edge.end()))
                for (int k = 0; k < n_y_j; k++) idx_j[k] = (n_x_j - 1) + k * n_x_j + (n_z_j - 1) * n_xy_j;

            else if ((edge.find(elem_j.glo_num[0 + 0 * n_x_j + 0 * n_xy_j]) != edge.end()) and (edge.find(elem_j.glo_num[0 + 0 * n_x_j + (n_z_j - 1) * n_xy_j]) != edge.end()))
                for (int k = 0; k < n_z_j; k++) idx_j[k] = 0 + 0 * n_x_j + k * n_xy_j;

            else if ((edge.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j + 0 * n_xy_j]) != edge.end()) and (edge.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j + (n_z_j - 1) * n_xy_j]) != edge.end()))
                for (int k = 0; k < n_z_j; k++) idx_j[k] = (n_x_j - 1) + 0 * n_x_j + k * n_xy_j;

            else if ((edge.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j + 0 * n_xy_j]) != edge.end()) and (edge.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j + (n_z_j - 1) * n_xy_j]) != edge.end()))
                for (int k = 0; k < n_z_j; k++) idx_j[k] = 0 + (n_y_j - 1) * n_x_j + k * n_xy_j;

            else if ((edge.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j + 0 * n_xy_j]) != edge.end()) and (edge.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j + (n_z_j - 1) * n_xy_j]) != edge.end()))
                for (int k = 0; k < n_z_j; k++) idx_j[k] = (n_x_j - 1) + (n_y_j - 1) * n_x_j + k * n_xy_j;
        }

        return std::pair<std::vector<int>, std::vector<int>>(idx_i, idx_j);
    };

    auto matching_face = [&](Element<DType> elem_i, Element<DType> elem_j, int fid)
    {
        int n_x_i = elem_i.n_x;
        int n_y_i = elem_i.n_y;
        int n_z_i = elem_i.n_z;
        int n_xy_i = n_x_i * n_y_i;

        int n_x_j = elem_j.n_x;
        int n_y_j = elem_j.n_y;
        int n_z_j = elem_j.n_z;
        int n_xy_j = n_x_j * n_y_j;

        std::set<long long> face;
        std::vector<int> idx_i(n_xy_i);
        std::vector<int> idx_j(n_xy_j);

        if (fid == 0)
        {
            face.insert(elem_i.glo_num[0 + 0 * n_x_i + 0 * n_xy_i]);
            face.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i + 0 * n_xy_i]);
            face.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i + 0 * n_xy_i]);
            face.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i + 0 * n_xy_i]);

            for (int j = 0; j < n_y_i; j++)
                for (int i = 0; i < n_x_i; i++)
                    idx_i[i + j * n_x_i] = i + j * n_x_i + 0 * n_xy_i;
        }
        else if (fid == 1)
        {
            face.insert(elem_i.glo_num[0 + 0 * n_x_i + (n_z_i - 1) * n_xy_i]);
            face.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i + (n_z_i - 1) * n_xy_i]);
            face.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i]);
            face.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i]);

            for (int j = 0; j < n_y_i; j++)
                for (int i = 0; i < n_x_i; i++)
                    idx_i[i + j * n_x_i] = i + j * n_x_i + (n_z_i - 1) * n_xy_i;
        }
        else if (fid == 2)
        {
            face.insert(elem_i.glo_num[0 + 0 * n_x_i + 0 * n_xy_i]);
            face.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i + 0 * n_xy_i]);
            face.insert(elem_i.glo_num[0 + 0 * n_x_i + (n_z_i - 1) * n_xy_i]);
            face.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i + (n_z_i - 1) * n_xy_i]);

            for (int k = 0; k < n_z_i; k++)
                for (int i = 0; i < n_x_i; i++)
                    idx_i[i + k * n_x_i] = i + 0 * n_x_i + k * n_xy_i;
        }
        else if (fid == 3)
        {
            face.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i + 0 * n_xy_i]);
            face.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i + 0 * n_xy_i]);
            face.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i]);
            face.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i]);

            for (int k = 0; k < n_z_i; k++)
                for (int i = 0; i < n_x_i; i++)
                    idx_i[i + k * n_x_i] = i + (n_y_i - 1) * n_x_i + k * n_xy_i;
        }
        else if (fid == 4)
        {
            face.insert(elem_i.glo_num[0 + 0 * n_x_i + 0 * n_xy_i]);
            face.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i + 0 * n_xy_i]);
            face.insert(elem_i.glo_num[0 + 0 * n_x_i + (n_z_i - 1) * n_xy_i]);
            face.insert(elem_i.glo_num[0 + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i]);

            for (int k = 0; k < n_z_i; k++)
                for (int j = 0; j < n_y_i; j++)
                    idx_i[j + k * n_y_i] = 0 + j * n_x_i + k * n_xy_i;
        }
        else if (fid == 5)
        {
            face.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i + 0 * n_xy_i]);
            face.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i + 0 * n_xy_i]);
            face.insert(elem_i.glo_num[(n_x_i - 1) + 0 * n_x_i + (n_z_i - 1) * n_xy_i]);
            face.insert(elem_i.glo_num[(n_x_i - 1) + (n_y_i - 1) * n_x_i + (n_z_i - 1) * n_xy_i]);

            for (int k = 0; k < n_z_i; k++)
                for (int j = 0; j < n_y_i; j++)
                    idx_i[j + k * n_y_i] = (n_x_i - 1) + j * n_x_i + k * n_xy_i;
        }

        if       ((face.find(elem_j.glo_num[0 + 0 * n_x_j + 0 * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j + 0 * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j + 0 * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j + 0 * n_xy_j]) != face.end()))
        {
            for (int j = 0; j < n_y_j; j++)
                for (int i = 0; i < n_x_j; i++)
                    idx_j[i + j * n_x_j] = i + j * n_x_j + 0 * n_xy_j;
        }

        else if ((face.find(elem_j.glo_num[0 + 0 * n_x_j + (n_z_j - 1) * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j + (n_z_j - 1) * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j + (n_z_j - 1) * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j + (n_z_j - 1) * n_xy_j]) != face.end()))
        {
            for (int j = 0; j < n_y_j; j++)
                for (int i = 0; i < n_x_j; i++)
                    idx_j[i + j * n_x_j] = i + j * n_x_j + (n_z_j - 1) * n_xy_j;
        }

        else if ((face.find(elem_j.glo_num[0 + 0 * n_x_j + 0 * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j + 0 * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[0 + 0 * n_x_j + (n_z_j - 1) * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j + (n_z_j - 1) * n_xy_j]) != face.end()))
        {
            for (int k = 0; k < n_z_j; k++)
                for (int i = 0; i < n_x_j; i++)
                    idx_j[i + k * n_x_j] = i + 0 * n_x_j + k * n_xy_j;
        }

        else if ((face.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j + 0 * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j + 0 * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j + (n_z_j - 1) * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j + (n_z_j - 1) * n_xy_j]) != face.end()))
        {
            for (int k = 0; k < n_z_j; k++)
                for (int i = 0; i < n_x_j; i++)
                    idx_j[i + k * n_x_j] = i + (n_y_j - 1) * n_x_j + k * n_xy_j;
        }

        else if ((face.find(elem_j.glo_num[0 + 0 * n_x_j + 0 * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j + 0 * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[0 + 0 * n_x_j + (n_z_j - 1) * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[0 + (n_y_j - 1) * n_x_j + (n_z_j - 1) * n_xy_j]) != face.end()))
        {
            for (int k = 0; k < n_z_j; k++)
                for (int j = 0; j < n_y_j; j++)
                    idx_j[j + k * n_y_j] = 0 + j * n_x_j + k * n_xy_j;
        }

        else if ((face.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j + 0 * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j + 0 * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[(n_x_j - 1) + 0 * n_x_j + (n_z_j - 1) * n_xy_j]) != face.end()) and 
                 (face.find(elem_j.glo_num[(n_x_j - 1) + (n_y_j - 1) * n_x_j + (n_z_j - 1) * n_xy_j]) != face.end()))
        {
            for (int k = 0; k < n_z_j; k++)
                for (int j = 0; j < n_y_j; j++)
                    idx_j[j + k * n_y_j] = (n_x_j - 1) + j * n_x_j + k * n_xy_j;
        }

        return std::pair<std::vector<int>, std::vector<int>>(idx_i, idx_j);
    };

    for (auto &region : { std::pair<std::vector<Element<DType>>&, CSR_Matrix<DType>&>(subdomain_region, subdomain_operator.Q), 
                          std::pair<std::vector<Element<DType>>&, CSR_Matrix<DType>&>(superdomain_region, superdomain_operator.Q) })
    {
        auto &elements = region.first;
        auto &Q = region.second;

        int num_elements = elements.size();
        int num_points = (num_elements > 0) ? elements[num_elements - 1].offset + elements[num_elements - 1].num_points : 0;
        int num_dofs = 0;

        for (auto &elem : elements)
            for (auto dof : elem.dof_num)
                num_dofs = std::max(num_dofs, (int)(dof));

        Q.initialize(num_points, num_dofs);

        for (auto &elem_i : elements)
        {
            int N_i = elem_i.poly_degree;
            int n_i = N_i + 1;

            // Vertices
            for (int vid = 0; vid < elem_i.num_points; vid++)
                if (elem_i.dof_num[vid] > 0)
                    Q.add_entry(elem_i.loc_num[vid], elem_i.dof_num[vid] - 1, 1.0);

            // Edges
            for (int eid = 0; eid < num_edges; eid++)
            {
                int e_j = -1;
                int N_j = N_i;
                int n_j = N_j + 1;

                for (auto e : elem_i.edge_conn[eid])
                {
                    if (elements[e].poly_degree < N_j)
                    {
                        e_j = e;
                        N_j = elements[e].poly_degree;
                        n_j = N_j + 1;
                    }
                }

                if (e_j >= 0)
                {
                    auto &elem_j = elements[e_j];

                    auto match = matching_edge(elem_i, elem_j, eid);
                    std::pair<int, int> idx(N_j, N_i);

                    for (int i = 1; i < n_i - 1; i++)
                        for (int j = 0; j < n_j; j++)
                            if (elem_j.dof_num[match.second[j]] > 0)
                                Q.add_entry(elem_i.loc_num[match.first[i]], elem_j.dof_num[match.second[j]] - 1, J_cf[idx].first[i * n_j + j]);
                }
            }

            // Faces
            if (dim == 3)
            {
                for (int fid = 0; fid < num_faces; fid++)
                {
                    for (auto e_j : elem_i.face_conn[fid])
                    {
                        Element<DType> &elem_j = elements[e_j];
                        int N_j = elem_j.poly_degree;
                        int n_j = N_j + 1;

                        if (N_i > N_j)
                        {
                            auto match = matching_face(elem_i, elem_j, fid);
                            std::pair<int, int> idx(N_j, N_i);

                            for (int j = 1; j < n_i - 1; j++)
                                for (int i = 1; i < n_i - 1; i++)
                                    for (int q = 0; q < n_j; q++)
                                        for (int p = 0; p < n_j; p++)
                                            if (elem_j.dof_num[match.second[p + q * n_j]] > 0)
                                                Q.add_entry(elem_i.loc_num[match.first[i + j * n_i]], elem_j.dof_num[match.second[p + q * n_j]] - 1, J_cf[idx].first[i * n_j + p] * J_cf[idx].first[j * n_j + q]);
                        }
                    }
                }
            }
        }

        Q.assemble();
    }

    subdomain_operator.Q.transpose(subdomain_operator.Qt);
    superdomain_operator.Q.transpose(superdomain_operator.Qt);

    // Subdomain stiffness operator setup
    std::unordered_map<int, int> level_degree;
    for (int l = 0; l < num_levels; l++) level_degree[poly_degree[l]] = l;

    subdomain_operator.num_dofs = 0;

    for (int e = 0; e < num_subdomain_elems; e++)
    {
        auto &elem = subdomain_region[e];

        subdomain_operator.num_dofs = std::max(subdomain_operator.num_dofs, (int)(*std::max_element(elem.dof_num.begin(), elem.dof_num.end())));
    }

    subdomain_operator.num_points = subdomain_operator.Q.num_rows;
    subdomain_operator.num_extended_dofs = subdomain_operator.Q.num_cols;

    subdomain_operator.element = device.malloc<int>(subdomain_operator.num_points);
    subdomain_operator.vertex = device.malloc<int>(subdomain_operator.num_points);
    subdomain_operator.level = device.malloc<int>(subdomain_operator.num_points);
    subdomain_operator.offset = device.malloc<int>(subdomain_operator.num_points);

    for (auto &elem : subdomain_region)
        for (int v = 0; v < elem.num_points; v++)
            ((int*)(work_hst[0].data()))[elem.offset + v] = subdomain_mapping[elem.id] - 1;

    subdomain_operator.element.copyFrom(work_hst[0].data(), subdomain_operator.num_points * sizeof(int));

    for (auto &elem : subdomain_region)
        for (int v = 0; v < elem.num_points; v++)
            ((int*)(work_hst[0].data()))[elem.offset + v] = v;

    subdomain_operator.vertex.copyFrom(work_hst[0].data(), subdomain_operator.num_points * sizeof(int));

    for (auto &elem : subdomain_region)
        for (int v = 0; v < elem.num_points; v++)
            ((int*)(work_hst[0].data()))[elem.offset + v] = level_degree[elem.poly_degree];

    subdomain_operator.level.copyFrom(work_hst[0].data(), subdomain_operator.num_points * sizeof(int));

    for (auto &elem : subdomain_region)
        for (int v = 0; v < elem.num_points; v++)
            ((int*)(work_hst[0].data()))[elem.offset + v] = elem.offset;

    subdomain_operator.offset.copyFrom(work_hst[0].data(), subdomain_operator.num_points * sizeof(int));

    // Superdomain stiffness operator setup
    PType &coarse_domain = domains[poly_degree[num_levels - 1]];
    std::vector<DType> geom_fact_coarse[NUM_GEOM_FACTS];

    proc_count[proc_id] = num_local_elements * num_vertices;
    MPI_Allgather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, proc_count.data(), 1, MPI_INT, MPI_COMM_WORLD);

    proc_offset[0] = 0;
    for (int p = 1; p < num_procs; p++) proc_offset[p] = proc_offset[p - 1] + proc_count[p - 1];

    for (int g = 0; g < NUM_GEOM_FACTS; g++)
    {
        geom_fact_coarse[g].resize(num_vertices * num_total_elements);

        for (auto &elem : coarse_domain.elements)
            for (int v = 0; v < elem.num_points; v++)
                geom_fact_coarse[g][proc_offset[proc_id] + (elem.id * num_vertices + v)] = elem.geom_fact[g][v];

        MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, geom_fact_coarse[g].data(), proc_count.data(), proc_offset.data(), (typeid(DType) == typeid(double)) ? MPI_DOUBLE : MPI_FLOAT, MPI_COMM_WORLD);
    }

    std::vector<long long> dof_num_coarse(num_vertices * num_total_elements);

    for (auto &elem : coarse_domain.elements)
        for (int v = 0; v < elem.num_points; v++)
            if (elem.dirichlet_mask[v] > 0.0)
                dof_num_coarse[proc_offset[proc_id] + (elem.id * num_vertices + v)] = elem.glo_num[v];

    MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, dof_num_coarse.data(), proc_count.data(), proc_offset.data(), MPI_LONG_LONG, MPI_COMM_WORLD);

    std::vector<long long> glo_num_coarse(dof_num_coarse);

    int num_coarse_dofs;

    {
        int size = num_vertices * num_total_elements;

        std::vector<std::pair<unsigned int, long long>> entries(size);

        for (int i = 0; i < size; i++)
        {
            entries[i].first = i;
            entries[i].second = dof_num_coarse[i];
        }

        std::sort(entries.begin(), entries.begin() + size, [&](const std::pair<unsigned int, long long> a, const std::pair<unsigned int, long long> b){ return a.second < b.second; });

        long long value = entries[0].second;
        long long rank = (value == 0) ? 0 : 1;

        entries[0].second = rank;

        for (int i = 1; i < size; i++)
        {
            auto &entry = entries[i];

            if (entry.second == value)
            {
                entry.second = rank;
            }
            else
            {
                rank += 1;
                value = entry.second;
                entry.second = rank;
            }
        }

        num_coarse_dofs = rank;

        std::sort(entries.begin(), entries.begin() + size);
        for (int i = 0; i < size; i++) dof_num_coarse[i] = entries[i].second;
    }

    Qt_coarse.initialize(num_coarse_dofs, num_total_elements * num_vertices);

    for (int e = 0; e < num_total_elements; e++)
        for (int v = 0; v < num_vertices; v++)
            if (dof_num_coarse[e * num_vertices + v] > 0)
                Qt_coarse.add_entry(dof_num_coarse[e * num_vertices + v] - 1, e * num_vertices + v, 1.0);

    Qt_coarse.assemble();

    std::vector<std::vector<DType>> D(dim, std::vector<DType>(num_vertices * num_vertices));

    if (dim == 2)
    {
        for (int k = 0; k < 2; k++)
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++)
                    D[0][(i + k * 2) * 4 + (j + k * 2)] = D_hat[num_levels - 1].first[i * 2 + j];

        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                for (int k = 0; k < 2; k++)
                    D[1][(i * 2 + k) * 4 + (j * 2 + k)] = D_hat[num_levels - 1].first[i * 2 + j];
    }
    else
    {
        for (int p = 0; p < 2; p++)
            for (int q = 0; q < 2; q++)
                for (int i = 0; i < 2; i++)
                    for (int j = 0; j < 2; j++)
                        D[0][(i + (p * 2 + q) * 2) * 8 + (j + (p * 2 + q) * 2)] = D_hat[num_levels - 1].first[i * 2 + j];

        for (int p = 0; p < 2; p++)
            for (int q = 0; q < 2; q++)
                for (int i = 0; i < 2; i++)
                    for (int j = 0; j < 2; j++)
                        D[1][(i * 8 + j) * 2 + ((p + p * 8) * (2 * 2) + (q + q * 8))] = D_hat[num_levels - 1].first[i * 2 + j];

        for (int p = 0; p < 2; p++)
            for (int q = 0; q < 2; q++)
                for (int i = 0; i < 2; i++)
                    for (int j = 0; j < 2; j++)
                        D[2][(i * 8 + j) * (2 * 2) + (p + q * 2) * (1 + 8)] = D_hat[num_levels - 1].first[i * 2 + j];
    }

    std::vector<std::vector<DType>> G(NUM_GEOM_FACTS, std::vector<DType>(num_vertices * num_vertices));
    std::vector<std::vector<DType>> GD(dim, std::vector<DType>(num_vertices * num_vertices));
    std::vector<DType> A_e(num_vertices * num_vertices);

    HYPRE_ParCSRMatrix A_coarse_csr;
    HYPRE_IJMatrix A_coarse;
    HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, num_coarse_dofs - 1, 0, num_coarse_dofs - 1, &A_coarse);
    HYPRE_IJMatrixSetObjectType(A_coarse, HYPRE_PARCSR);
    HYPRE_IJMatrixInitialize_v2(A_coarse, HYPRE_MEMORY_HOST);

    for (int e = 0; e < num_total_elements; e++)
    {
        if (dim == 2)
        {
            for (int g = 0; g < NUM_GEOM_FACTS; g++)
                for (int v = 0; v < 4; v++)
                    G[g][v * 4 + v] = geom_fact_coarse[g][e * num_vertices + v];

            for (int i = 0; i < 4; i++)
            {
                for (int j = 0; j < 4; j++)
                {
                    DType GD_1 = 0.0;
                    DType GD_2 = 0.0;

                    for (int k = 0; k < 4; k++)
                    {
                        GD_1 += G[0][i * 4 + k] * D[0][k * 4 + j] + G[2][i * 4 + k] * D[1][k * 4 + j];
                        GD_2 += G[2][i * 4 + k] * D[0][k * 4 + j] + G[1][i * 4 + k] * D[1][k * 4 + j];
                    }

                    GD[0][i * 4 + j] = GD_1;
                    GD[1][i * 4 + j] = GD_2;
                }
            }

            std::fill(A_e.begin(), A_e.end(), 0.0);

            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                    for (int k = 0; k < 4; k++)
                        A_e[i * 4 + j] += D[0][k * 4 + i] * GD[0][k * 4 + j] + D[1][k * 4 + i] * GD[1][k * 4 + j];
        }
        else
        {
            for (int g = 0; g < NUM_GEOM_FACTS; g++)
                for (int v = 0; v < 8; v++)
                    G[g][v * 8 + v] = geom_fact_coarse[g][e * num_vertices + v];

            for (int i = 0; i < 8; i++)
            {
                for (int j = 0; j < 8; j++)
                {
                    DType GD_1 = 0.0;
                    DType GD_2 = 0.0;
                    DType GD_3 = 0.0;

                    for (int k = 0; k < 8; k++)
                    {
                        GD_1 += G[0][i * 8 + k] * D[0][k * 8 + j] + G[3][i * 8 + k] * D[1][k * 8 + j] + G[4][i * 8 + k] * D[2][k * 8 + j];
                        GD_2 += G[3][i * 8 + k] * D[0][k * 8 + j] + G[1][i * 8 + k] * D[1][k * 8 + j] + G[5][i * 8 + k] * D[2][k * 8 + j];
                        GD_3 += G[4][i * 8 + k] * D[0][k * 8 + j] + G[5][i * 8 + k] * D[1][k * 8 + j] + G[2][i * 8 + k] * D[2][k * 8 + j];
                    }

                    GD[0][i * 8 + j] = GD_1;
                    GD[1][i * 8 + j] = GD_2;
                    GD[2][i * 8 + j] = GD_3;
                }
            }

            std::fill(A_e.begin(), A_e.end(), 0.0);

            for (int i = 0; i < 8; i++)
                for (int j = 0; j < 8; j++)
                    for (int k = 0; k < 8; k++)
                        A_e[i * 8 + j] += D[0][k * 8 + i] * GD[0][k * 8 + j] + D[1][k * 8 + i] * GD[1][k * 8 + j] + D[2][k * 8 + i] * GD[2][k * 8 + j];
        }

        int one = 1;
        int row;
        int col;
        DType val;

        for (int i = 0; i < num_vertices; i++)
        {
            for (int j = 0; j < num_vertices; j++)
            {
                row = dof_num_coarse[e * num_vertices + i] - 1;
                col = dof_num_coarse[e * num_vertices + j] - 1;
                val = A_e[i * num_vertices + j];

                if ((row >= 0) and (col >= 0))
                    if (std::abs(val) > epsilon)
                        HYPRE_IJMatrixAddToValues(A_coarse, 1, &one, &row, &col, &val);
            }
        }
    }

    HYPRE_IJMatrixAssemble(A_coarse);
    HYPRE_IJMatrixGetObject(A_coarse, (void**)(&A_coarse_csr));

    HYPRE_Solver amg_coarse;
    HYPRE_BoomerAMGCreate(&(amg_coarse));
    HYPRE_BoomerAMGSetCoarsenType(amg_coarse, 10);
    HYPRE_BoomerAMGSetInterpType(amg_coarse, 6);
    HYPRE_BoomerAMGSetMaxCoarseSize(amg_coarse, 1);
    HYPRE_BoomerAMGSetStrongThreshold(amg_coarse, 0.25);
    HYPRE_BoomerAMGSetPrintLevel(amg_coarse, 0);
    HYPRE_BoomerAMGSetup(amg_coarse, A_coarse_csr, NULL, NULL);

    std::vector<int> dof_marker(num_coarse_dofs);

    for (int e = 0; e < num_subdomain_elems; e++)
    {
        int eid = subdomain_region[e].id;

        for (int v = 0; v < num_vertices; v++)
        {
            long long dof = dof_num_coarse[eid * num_vertices + v];
            long long glo = glo_num_coarse[eid * num_vertices + v];

            if (dof > 0)
                dof_marker[dof - 1] = 1;

            if (interface_glo_num.find(glo) != interface_glo_num.end())
                dof_marker[dof - 1] = 2;
        }
    }

    for (int e = num_subdomain_elems; e < num_subdomain_extended_elems; e++)
    {
        int eid = subdomain_region[e].id;

        for (int v = 0; v < num_vertices; v++)
        {
            long long dof = dof_num_coarse[eid * num_vertices + v];

            if (dof > 0)
                if (dof_marker[dof - 1] == 0)
                    dof_marker[dof - 1] = 3;
        }
    }

    for (int e = num_superdomain_elems; e < num_superdomain_extended_elems; e++)
    {
        int eid = superdomain_region[e].id;

        for (int v = 0; v < num_vertices; v++)
        {
            long long dof = dof_num_coarse[eid * num_vertices + v];

            if (dof > 0)
                if (dof_marker[dof - 1] == 1)
                    dof_marker[dof - 1] = 4;
        }
    }

    HYPRE_ParCSRMatrix P_sup_csr;
    HYPRE_IJMatrix P_sup;
    HYPRE_ParCSRMatrix A_sup_csr;
    HYPRE_IJMatrix A_sup;
    std::vector<int> dof_sup;

    {
        hypre_ParAMGData *amg_data = (hypre_ParAMGData*)(amg_coarse);

        HYPRE_Int num_levels = hypre_ParAMGDataNumLevels(amg_data);
        hypre_ParCSRMatrix **A = hypre_ParAMGDataAArray(amg_data);
        hypre_ParCSRMatrix **P = hypre_ParAMGDataPArray(amg_data);

        std::vector<int> num_nodes(num_levels);
        std::vector<std::vector<DType>> D(num_levels);

        for (int l = 0; l < num_levels; l++)
        {
            num_nodes[l] = hypre_CSRMatrixNumRows(hypre_ParCSRMatrixDiag(A[l]));
            D[l].resize(num_nodes[l]);
        }

        for (int i = 0; i < num_nodes[0]; i++)
            if (dof_marker[i] > 0)
                D[0][i] = 1.0;

        int num_comp_levels = 0;

        for (int l = 0; l < num_levels; l++)
        {
            num_comp_levels = l + 1;

            HYPRE_Int *A_ptr = hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(A[l]));
            HYPRE_Int *A_col = hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(A[l]));

            memcpy(work_hst[0].data(), D[l].data(), num_nodes[l] * sizeof(DType));

            for (int nu = 0; nu < superdomain_overlap; nu++)
            {
                for (int row = 0; row < num_nodes[l]; row++)
                {
                    DType val = 0.0;

                    for (int ptr = A_ptr[row]; ptr < A_ptr[row + 1]; ptr++)
                        val += work_hst[0][A_col[ptr]];

                    work_hst[1][row] = val;
                }

                memcpy(work_hst[0].data(), work_hst[1].data(), num_nodes[l] * sizeof(DType));
            }

            if (superdomain_overlap == 0) superdomain_overlap = 1;

            if (l == num_levels - 1)
                for (int i = 0; i < num_nodes[l]; i++)
                    work_hst[0][i] = 1.0;

            for (int i = 0; i < num_nodes[l]; i++)
                if ((D[l][i] == 0.0) and (work_hst[0][i] > 0.0))
                    D[l][i] = 2.0;

            bool is_coarsest = true;

            for (int i = 0; i < num_nodes[l]; i++)
            {
                if (D[l][i] == 0.0)
                {
                    is_coarsest = false;
                    break;
                }
            }

            if (is_coarsest) break;

            if (l < num_levels - 1)
            {
                HYPRE_Int  *P_ptr = hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(P[l]));
                HYPRE_Int  *P_col = hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(P[l]));

                for (int row = 0; row < num_nodes[l]; row++)
                    if (P_ptr[row + 1] - P_ptr[row] == 1)
                        if (D[l][row] > 0.0)
                            D[l + 1][P_col[P_ptr[row]]] = 1.0;
            }
        }

        std::vector<int> num_local(num_comp_levels);
        std::vector<int> num_overlap(num_comp_levels);
        std::vector<int> num_remaining(num_comp_levels);
        std::vector<int> num_comp_overlap(num_comp_levels);

        for (int l = 0; l < num_comp_levels; l++)
        {
            for (int i = 0; i < num_nodes[l]; i++)
            {
                if (D[l][i] == 1.0) num_local[l]++;
                if (D[l][i] == 2.0) num_overlap[l]++;
                if (D[l][i] == 0.0) num_remaining[l]++;
            }

            num_comp_overlap[l] = num_overlap[l];
        }

        num_comp_overlap[0] += num_local[0];

        // Tag coarse nodes with their fine counterpart
        std::vector<std::vector<int>> nodes_to_fine(num_comp_levels);

        nodes_to_fine[0].resize(num_nodes[0]);
        for (int i = 0; i < num_nodes[0]; i++) nodes_to_fine[0][i] = i;

        for (int l = 0; l < num_comp_levels - 1; l++)
        {
            nodes_to_fine[l + 1].resize(num_nodes[l + 1]);

            HYPRE_Int *P_ptr = hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(P[l]));
            HYPRE_Int *P_col = hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(P[l]));

            for (int row = 0; row < num_nodes[l]; row++)
                if (P_ptr[row + 1] - P_ptr[row] == 1)
                    nodes_to_fine[l + 1][P_col[P_ptr[row]]] = nodes_to_fine[l][row];
        }

        // Tag level nodes with the respective DOF in the composite graph
        std::vector<std::vector<int>> nodes_to_dofs(num_comp_levels);

        for (int l = 0; l < num_comp_levels; l++)
        {
            nodes_to_dofs[l].resize(num_nodes[l]);
            std::fill(nodes_to_dofs[l].begin(), nodes_to_dofs[l].end(), - 1);
        }

        int dof_start;
        int dof_end = 0;

        for (auto marker : { 1, 2, 3, 4 })
        {
            dof_start = dof_end;
            dof_end = dof_start;

            for (int i = 0; i < num_nodes[0]; i++)
                if (dof_marker[i] == marker)
                    nodes_to_dofs[0][i] = dof_end++;
        }

        dof_start = num_local[0];
        dof_end = dof_start;

        for (int i = 0; i < num_nodes[0]; i++)
            if (D[0][i] == 2.0)
                nodes_to_dofs[0][i] = dof_end++;

        int offset = num_local[0] + num_overlap[0];

        for (int l = 0; l < num_comp_levels - 1; l++)
            for (int i = 0; i < num_nodes[l + 1]; i++)
                if (D[l + 1][i] == 2.0)
                    nodes_to_dofs[0][nodes_to_fine[l + 1][i]] = offset++;

        for (int l = 0; l < num_comp_levels - 1; l++)
        {
            nodes_to_fine[l + 1].resize(num_nodes[l + 1]);

            HYPRE_Int *P_ptr = hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(P[l]));
            HYPRE_Int *P_col = hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(P[l]));

            for (int row = 0; row < num_nodes[l]; row++)
                if (P_ptr[row + 1] - P_ptr[row] == 1)
                    nodes_to_dofs[l + 1][P_col[P_ptr[row]]] = nodes_to_dofs[l][row];
        }

        int num_dofs = offset;

        // Coarse to fine interpolator
        std::vector<HYPRE_IJMatrix> P_c_mat(num_comp_levels - 1);
        std::vector<HYPRE_ParCSRMatrix> P_c_csr(num_comp_levels - 1);

        std::vector<HYPRE_IJMatrix> R_c_mat(num_comp_levels - 1);
        std::vector<HYPRE_ParCSRMatrix> R_c_csr(num_comp_levels - 1);

        for (int l = num_comp_levels - 1; l > 0; l--)
        {
            HYPRE_Int  *P_ptr = hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(P[l - 1]));
            HYPRE_Int  *P_col = hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(P[l - 1]));
            HYPRE_Real *P_val = hypre_CSRMatrixData(hypre_ParCSRMatrixDiag(P[l - 1]));

            // Mark fine nodes 
            std::vector<int> fine_nodes(num_nodes[l - 1], - 1);

            if (l - 1 == 0)
            {
                dof_end = 0;

                for (auto marker : { 1, 2, 3, 4 })
                {
                    dof_start = dof_end;
                    dof_end = dof_start;

                    for (int i = 0; i < num_nodes[0]; i++)
                        if (dof_marker[i] == marker)
                            fine_nodes[i] = dof_end++;
                }

                dof_start = dof_end;
                dof_end = dof_start;

                for (int i = 0; i < num_nodes[0]; i++)
                    if (D[0][i] == 2.0)
                        fine_nodes[i] = dof_end++;

                dof_start = num_local[0] + num_overlap[0];
                dof_end = dof_start;

                for (int i = 0; i < num_nodes[0]; i++)
                    if (D[0][i] == 0.0)
                        fine_nodes[i] = dof_end++;
            }
            else
            {
                dof_start = 0;
                dof_end = dof_start;

                for (int i = 0; i < num_nodes[l - 1]; i++)
                    if (D[l - 1][i] == 2.0)
                        fine_nodes[i] = dof_end++;

                dof_start = num_overlap[l - 1];
                dof_end = dof_start;

                for (int i = 0; i < num_nodes[l - 1]; i++)
                    if (D[l - 1][i] == 0.0)
                        fine_nodes[i] = dof_end++;
            }

            // Mark coarse nodes 
            std::vector<int> coarse_nodes(num_nodes[l - 0], - 1);

            if (l - 1 == 0)
            {
                dof_start = num_local[0] + num_overlap[0];
                dof_end = dof_start;
            }
            else
            {
                dof_start = num_overlap[l - 1];
                dof_end = dof_start;
            }

            for (int i = 0; i < num_nodes[l - 0]; i++)
                if ((D[l - 0][i] == 2.0) or (D[l - 0][i] == 0.0))
                    coarse_nodes[i] = dof_end++;

            for (int row = 0; row < num_nodes[l - 1]; row++)
            {
                if (P_ptr[row + 1] - P_ptr[row] == 1)
                {
                    if (l - 1 == 0)
                    {
                        if (fine_nodes[row] < (num_local[0] + num_overlap[0]))
                            coarse_nodes[P_col[P_ptr[row]]] = fine_nodes[row];
                    }
                    else
                    {
                        if (fine_nodes[row] < num_overlap[l - 1])
                            coarse_nodes[P_col[P_ptr[row]]] = fine_nodes[row];
                    }
                }
            }

            // Construct level interpolator
            int one = 1;

            int num_fine = num_overlap[l - 1];
            if (l - 1 == 0) num_fine += num_local[l - 1];

            if (l - 1 == 0)
            {
                int num_rows = num_nodes[l - 1];
                int num_cols = num_local[l - 1] + num_overlap[l - 1] + num_overlap[l - 0] + num_remaining[l - 0];

                HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, num_rows - 1, 0, num_cols - 1, &(P_c_mat[l - 1]));
            }
            else
            {
                int num_rows = num_overlap[l - 1] + num_remaining[l - 1];
                int num_cols = num_overlap[l - 1] + num_overlap[l - 0] + num_remaining[l - 0];

                HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, num_rows - 1, 0, num_cols - 1, &(P_c_mat[l - 1]));
            }

            HYPRE_IJMatrixSetObjectType(P_c_mat[l - 1], HYPRE_PARCSR);
            HYPRE_IJMatrixInitialize_v2(P_c_mat[l - 1], HYPRE_MEMORY_HOST);

            for (int row = 0; row < num_nodes[l - 1]; row++)
            {
                if (fine_nodes[row] < 0) continue;

                if (fine_nodes[row] < num_fine)
                {
                    DType val = 1.0;
                    HYPRE_IJMatrixAddToValues(P_c_mat[l - 1], 1, &one, &(fine_nodes[row]), &(fine_nodes[row]), &val);
                }
                else
                {
                    for (int ptr = P_ptr[row]; ptr < P_ptr[row + 1]; ptr++)
                    {
                        int col = P_col[ptr];
                        DType val = P_val[ptr];

                        if (coarse_nodes[col] >= 0)
                            HYPRE_IJMatrixAddToValues(P_c_mat[l - 1], 1, &one, &(fine_nodes[row]), &(coarse_nodes[col]), &val);
                    }
                }
            }

            HYPRE_IJMatrixAssemble(P_c_mat[l - 1]);
            HYPRE_IJMatrixGetObject(P_c_mat[l - 1], (void**)(&(P_c_csr[l - 1])));

            // Construct mapping to original ordering
            int num_rows = 0;

            for (int i = 0; i < num_nodes[l - 1]; i++)
                if (fine_nodes[i] >= 0)
                    num_rows++;

            HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, num_rows - 1, 0, num_rows - 1, &(R_c_mat[l - 1]));
            HYPRE_IJMatrixSetObjectType(R_c_mat[l - 1], HYPRE_PARCSR);
            HYPRE_IJMatrixInitialize_v2(R_c_mat[l - 1], HYPRE_MEMORY_HOST);

            int num_cols = 0;

            for (int i = 0; i < num_nodes[l - 1]; i++)
            {
                if (fine_nodes[i] >= 0)
                {
                    DType val = 1.0;
                    HYPRE_IJMatrixAddToValues(R_c_mat[l - 1], 1, &one, &num_cols, &(fine_nodes[i]), &val);
                    num_cols++;
                }
            }

            HYPRE_IJMatrixAssemble(R_c_mat[l - 1]);
            HYPRE_IJMatrixGetObject(R_c_mat[l - 1], (void**)(&(R_c_csr[l - 1])));
        }

        // Construct composite to global interpolator
        HYPRE_IJMatrix P_mat;
        HYPRE_ParCSRMatrix P_csr;

        if (num_comp_levels > 1)
        {
            for (int l = num_comp_levels - 2; l > 0; l--)
            {
                // Separate components
                HYPRE_Int  num_rows = hypre_CSRMatrixNumRows(hypre_ParCSRMatrixDiag(P_c_csr[l - 1]));
                HYPRE_Int  num_cols = hypre_CSRMatrixNumCols(hypre_ParCSRMatrixDiag(P_c_csr[l - 1]));
                HYPRE_Int  *P_c_ptr = hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(P_c_csr[l - 1]));
                HYPRE_Int  *P_c_col = hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(P_c_csr[l - 1]));
                HYPRE_Real *P_c_val = hypre_CSRMatrixData(hypre_ParCSRMatrixDiag(P_c_csr[l - 1]));

                HYPRE_ParCSRMatrix P_c_lm1_21_csr;
                HYPRE_IJMatrix P_c_lm1_21_mat;
                HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, num_rows - num_comp_overlap[l - 1] - 1, 0, num_comp_overlap[l - 1] - 1, &P_c_lm1_21_mat);
                HYPRE_IJMatrixSetObjectType(P_c_lm1_21_mat, HYPRE_PARCSR);
                HYPRE_IJMatrixInitialize_v2(P_c_lm1_21_mat, HYPRE_MEMORY_HOST);

                HYPRE_ParCSRMatrix P_c_lm1_22_csr;
                HYPRE_IJMatrix P_c_lm1_22_mat;
                HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, num_rows - num_comp_overlap[l - 1] - 1, 0, num_cols - num_comp_overlap[l - 1] - 1, &P_c_lm1_22_mat);
                HYPRE_IJMatrixSetObjectType(P_c_lm1_22_mat, HYPRE_PARCSR);
                HYPRE_IJMatrixInitialize_v2(P_c_lm1_22_mat, HYPRE_MEMORY_HOST);

                for (int i = num_comp_overlap[l - 1]; i < num_rows; i++)
                {
                    for (int ptr = P_c_ptr[i]; ptr < P_c_ptr[i + 1]; ptr++)
                    {
                        int one = 1;
                        int row = i - num_comp_overlap[l - 1];
                        int col = P_c_col[ptr];
                        DType val = P_c_val[ptr];

                        if (col < num_comp_overlap[l - 1])
                        {
                            HYPRE_IJMatrixAddToValues(P_c_lm1_21_mat, 1, &one, &row, &col, &val);
                        }
                        else
                        {
                            col -= num_comp_overlap[l - 1];
                            HYPRE_IJMatrixAddToValues(P_c_lm1_22_mat, 1, &one, &row, &col, &val);
                        }
                    }
                }

                HYPRE_IJMatrixAssemble(P_c_lm1_21_mat);
                HYPRE_IJMatrixGetObject(P_c_lm1_21_mat, (void**)(&P_c_lm1_21_csr));

                HYPRE_IJMatrixAssemble(P_c_lm1_22_mat);
                HYPRE_IJMatrixGetObject(P_c_lm1_22_mat, (void**)(&P_c_lm1_22_csr));

                // Apply interpolation to lower right block
                HYPRE_ParCSRMatrix Rl_Pl = hypre_ParCSRMatMatHost(R_c_csr[l], P_c_csr[l]);
                HYPRE_ParCSRMatrix Plm1_Rl_Pl = hypre_ParCSRMatMatHost(P_c_lm1_22_csr, Rl_Pl);

                num_cols = num_comp_overlap[l - 1] + hypre_CSRMatrixNumCols(hypre_ParCSRMatrixDiag(Plm1_Rl_Pl));

                HYPRE_IJMatrixDestroy(P_c_mat[l - 1]);
                HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, num_rows - 1, 0, num_cols - 1, &(P_c_mat[l - 1]));
                HYPRE_IJMatrixSetObjectType(P_c_mat[l - 1], HYPRE_PARCSR);
                HYPRE_IJMatrixInitialize_v2(P_c_mat[l - 1], HYPRE_MEMORY_HOST);

                for (int row = 0; row < num_comp_overlap[l - 1]; row++)
                {
                    int one = 1;
                    DType val = 1.0;
                    HYPRE_IJMatrixAddToValues(P_c_mat[l - 1], 1, &one, &row, &row, &val);
                }

                {
                    HYPRE_Int  *mat_ptr = hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(P_c_lm1_21_csr));
                    HYPRE_Int  *mat_col = hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(P_c_lm1_21_csr));
                    HYPRE_Real *mat_val = hypre_CSRMatrixData(hypre_ParCSRMatrixDiag(P_c_lm1_21_csr));

                    for (int i = 0; i < num_rows - num_comp_overlap[l - 1]; i++)
                    {
                        for (int ptr = mat_ptr[i]; ptr < mat_ptr[i + 1]; ptr++)
                        {
                            int one = 1;
                            int row = i + num_comp_overlap[l - 1];
                            int col = mat_col[ptr];
                            DType val = mat_val[ptr];

                            HYPRE_IJMatrixAddToValues(P_c_mat[l - 1], 1, &one, &row, &col, &val);
                        }
                    }
                }

                {
                    HYPRE_Int  *mat_ptr = hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(Plm1_Rl_Pl));
                    HYPRE_Int  *mat_col = hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(Plm1_Rl_Pl));
                    HYPRE_Real *mat_val = hypre_CSRMatrixData(hypre_ParCSRMatrixDiag(Plm1_Rl_Pl));

                    for (int i = 0; i < num_rows - num_comp_overlap[l - 1]; i++)
                    {
                        for (int ptr = mat_ptr[i]; ptr < mat_ptr[i + 1]; ptr++)
                        {
                            int one = 1;
                            int row = i + num_comp_overlap[l - 1];
                            int col = mat_col[ptr] + num_comp_overlap[l - 1];
                            DType val = mat_val[ptr];

                            HYPRE_IJMatrixAddToValues(P_c_mat[l - 1], 1, &one, &row, &col, &val);
                        }
                    }
                }

                HYPRE_IJMatrixAssemble(P_c_mat[l - 1]);
                HYPRE_IJMatrixGetObject(P_c_mat[l - 1], (void**)(&(P_c_csr[l - 1])));

                HYPRE_IJMatrixDestroy(P_c_lm1_21_mat);
                HYPRE_IJMatrixDestroy(P_c_lm1_22_mat);
                HYPRE_ParCSRMatrixDestroy(Rl_Pl);
                HYPRE_ParCSRMatrixDestroy(Plm1_Rl_Pl);
            }

            P_csr = hypre_ParCSRMatMatHost(R_c_csr[0], P_c_csr[0]);
        }
        else
        {
            HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, num_dofs - 1, 0, num_dofs - 1, &P_mat);
            HYPRE_IJMatrixSetObjectType(P_mat, HYPRE_PARCSR);
            HYPRE_IJMatrixInitialize_v2(P_mat, HYPRE_MEMORY_HOST);

            for (int i = 0; i < num_dofs; i++)
            {
                int one = 1;
                int row = i;
                int col = nodes_to_dofs[0][i];
                DType val = 1.0;

                HYPRE_IJMatrixAddToValues(P_mat, 1, &one, &row, &col, &val);
            }

            HYPRE_IJMatrixAssemble(P_mat);
            HYPRE_IJMatrixGetObject(P_mat, (void**)(&P_csr));
        }

        for (int l = 0; l < num_comp_levels - 1; l++)
        {
            HYPRE_IJMatrixDestroy(P_c_mat[l]);
            HYPRE_IJMatrixDestroy(R_c_mat[l]);
        }

        // Construct composite operators
        HYPRE_ParCSRMatrix PtA = hypre_ParCSRTMatMatKTHost(P_csr, A[0], 0);
        HYPRE_ParCSRMatrix PtAP = hypre_ParCSRMatMatHost(PtA, P_csr);

        int num_markers = 5;
        std::vector<int> marker_offset(num_markers);
        std::vector<int> marker_count(num_markers);

        for (int i = 0; i < num_nodes[0]; i++)
        {
            if (dof_marker[i] == 1) marker_count[0]++;
            if (dof_marker[i] == 2) marker_count[1]++;
            if (dof_marker[i] == 3) marker_count[2]++;
            if (dof_marker[i] == 4) marker_count[3]++;
        }

        for (int m = 1; m < num_markers; m++) marker_offset[m] = marker_offset[m - 1] + marker_count[m - 1];
        marker_count[4] = hypre_CSRMatrixNumRows(hypre_ParCSRMatrixDiag(PtAP)) - marker_offset[4];

        std::vector<int> R_sup(hypre_CSRMatrixNumRows(hypre_ParCSRMatrixDiag(PtAP)), - 1);

        int dof = 0;
        for (int i = marker_offset[1]; i < marker_offset[3]; i++) R_sup[i] = dof++;
        for (int i = marker_offset[4]; i < hypre_CSRMatrixNumRows(hypre_ParCSRMatrixDiag(PtAP)); i++) R_sup[i] = dof++;
        for (int i = marker_offset[3]; i < marker_offset[4]; i++) R_sup[i] = dof++;

        int num_rows = marker_count[0] + marker_count[1] + marker_count[2] + marker_count[3] + marker_count[4];
        int num_cols = marker_count[1] + marker_count[2] + marker_count[3] + marker_count[4];

        HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, num_cols - 1, 0, num_cols - 1, &A_sup);
        HYPRE_IJMatrixSetObjectType(A_sup, HYPRE_PARCSR);
        HYPRE_IJMatrixInitialize_v2(A_sup, HYPRE_MEMORY_HOST);

        HYPRE_Int  *PtAP_ptr = hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(PtAP));
        HYPRE_Int  *PtAP_col = hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(PtAP));
        HYPRE_Real *PtAP_val = hypre_CSRMatrixData(hypre_ParCSRMatrixDiag(PtAP));

        for (int i = 0; i < num_rows; i++)
        {
            for (int ptr = PtAP_ptr[i]; ptr < PtAP_ptr[i + 1]; ptr++)
            {
                int one = 1;
                int row = R_sup[i];
                int col = R_sup[PtAP_col[ptr]];
                DType val = PtAP_val[ptr];

                if ((row >= 0) and (col >= 0))
                    HYPRE_IJMatrixAddToValues(A_sup, 1, &one, &row, &col, &val);
            }
        }

        HYPRE_IJMatrixAssemble(A_sup);
        HYPRE_IJMatrixGetObject(A_sup, (void**)(&A_sup_csr));

        num_rows = hypre_CSRMatrixNumRows(hypre_ParCSRMatrixDiag(P_csr));
        num_cols = dof;

        HYPRE_Int  *P_ptr = hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(P_csr));
        HYPRE_Int  *P_col = hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(P_csr));
        HYPRE_Real *P_val = hypre_CSRMatrixData(hypre_ParCSRMatrixDiag(P_csr));

        HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, num_rows - 1, 0, num_cols - 1, &P_sup);
        HYPRE_IJMatrixSetObjectType(P_sup, HYPRE_PARCSR);
        HYPRE_IJMatrixInitialize_v2(P_sup, HYPRE_MEMORY_HOST);

        for (int row = 0; row < num_rows; row++)
        {
            for (int ptr = P_ptr[row]; ptr < P_ptr[row + 1]; ptr++)
            {
                int one = 1;
                int col = R_sup[P_col[ptr]];
                DType val = P_val[ptr];

                if (col >= 0)
                    HYPRE_IJMatrixAddToValues(P_sup, 1, &one, &row, &col, &val);
            }
        }

        HYPRE_IJMatrixAssemble(P_sup);
        HYPRE_IJMatrixGetObject(P_sup, (void**)(&P_sup_csr));

        dof_sup.resize(num_nodes[0]);
        memcpy(dof_sup.data(), nodes_to_dofs[0].data(), num_nodes[0] * sizeof(int));

        for (int i = 0; i < num_nodes[0]; i++)
            if (dof_marker[i] == 1)
                dof_sup[i] = - 1;

        int dof_max = *std::max_element(nodes_to_dofs[0].begin(), nodes_to_dofs[0].end());

        for (int i = 0; i < num_nodes[0]; i++)
            if (dof_marker[i] == 4)
                dof_sup[i] += dof_max;

        {
            int size = num_nodes[0];
            std::vector<std::pair<int, int>> entries(size);

            for (int i = 0; i < size; i++)
            {
                entries[i].first = i;
                entries[i].second = dof_sup[i];
            }

            std::sort(entries.begin(), entries.begin() + size, [&](const std::pair<int, int> a, const std::pair<int, int> b){ return a.second < b.second; });

            int value = entries[0].second;
            int rank = (value == -1) ? 0 : 1;

            entries[0].second = rank;

            for (int i = 1; i < size; i++)
            {
                auto &entry = entries[i];

                if (entry.second == value)
                {
                    entry.second = rank;
                }
                else
                {
                    rank += 1;
                    value = entry.second;
                    entry.second = rank;
                }
            }

            std::sort(entries.begin(), entries.begin() + size);
            for (int i = 0; i < size; i++) dof_sup[i] = entries[i].second;
        }

        if (num_comp_levels > 1)
            HYPRE_ParCSRMatrixDestroy(P_csr);
        else
            HYPRE_IJMatrixDestroy(P_mat);

        HYPRE_ParCSRMatrixDestroy(PtA);
        HYPRE_ParCSRMatrixDestroy(PtAP);

        HYPRE_BoomerAMGDestroy(amg_coarse);

        // Construct superdomain operator structure
        num_rows = hypre_CSRMatrixNumRows(hypre_ParCSRMatrixDiag(P_sup_csr));
        num_cols = hypre_CSRMatrixNumCols(hypre_ParCSRMatrixDiag(P_sup_csr));

        superdomain_operator.num_dofs = hypre_CSRMatrixNumCols(hypre_ParCSRMatrixDiag(A_sup_csr)) - marker_count[3];
        superdomain_operator.num_extended_dofs = num_cols;
        superdomain_operator.num_points = superdomain_operator.Q.num_rows;

        superdomain_operator.Pt.initialize(num_cols, num_rows);

        {
            HYPRE_Int  *mat_ptr = hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(P_sup_csr));
            HYPRE_Int  *mat_col = hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(P_sup_csr));
            HYPRE_Real *mat_val = hypre_CSRMatrixData(hypre_ParCSRMatrixDiag(P_sup_csr));

            for (int row = 0; row < num_rows; row++)
                for (int ptr = mat_ptr[row]; ptr < mat_ptr[row + 1]; ptr++)
                    superdomain_operator.Pt.add_entry(mat_col[ptr], row, mat_val[ptr]);
        }

        superdomain_operator.Pt.assemble();

        superdomain_operator.A.initialize(num_cols, num_cols);

        {
            HYPRE_Int  *mat_ptr = hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(A_sup_csr));
            HYPRE_Int  *mat_col = hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(A_sup_csr));
            HYPRE_Real *mat_val = hypre_CSRMatrixData(hypre_ParCSRMatrixDiag(A_sup_csr));

            for (int row = 0; row < num_cols; row++)
                for (int ptr = mat_ptr[row]; ptr < mat_ptr[row + 1]; ptr++)
                    superdomain_operator.A.add_entry(row, mat_col[ptr], mat_val[ptr]);
        }

        superdomain_operator.A.assemble();
    }

    HYPRE_IJMatrixDestroy(P_sup);
    HYPRE_IJMatrixDestroy(A_sup);

    // Interface operator
    num_interface_dofs = (int)(interface_glo_num.size());
    num_dofs = subdomain_operator.num_dofs + superdomain_operator.num_dofs - num_interface_dofs;

    std::unordered_map<long long, long long> subdomain_dof_mapping;

    for (int e = 0; e < num_subdomain_elems; e++)
    {
        auto &elem = subdomain_region[e];

        for (int v = 0; v < elem.num_points; v++)
            if (elem.dof_num[v] > 0)
                subdomain_dof_mapping[elem.dof_num[v]] = elem.dof_num[v];
    }

    for (int e = num_subdomain_elems; e < num_subdomain_extended_elems; e++)
    {
        auto &elem = subdomain_region[e];

        for (int v = 0; v < elem.num_points; v++)
        {
            int dof = dof_num_coarse[elem.id * num_vertices + v];

            if (dof > 0)
            {
                dof -= 1;

                if (dof_sup[dof] > 0)
                    subdomain_dof_mapping[elem.dof_num[v]] = dof_sup[dof] + (subdomain_operator.num_dofs - num_interface_dofs);
            }
        }
    }

    std::unordered_map<long long, long long> superdomain_dof_mapping;

    for (int e = 0; e < num_superdomain_elems; e++)
    {
        auto &elem = superdomain_region[e];

        for (int v = 0; v < elem.num_points; v++)
        {
            int dof = dof_num_coarse[elem.id * num_vertices + v];

            if (dof > 0)
            {
                dof -= 1;
                superdomain_dof_mapping[dof_sup[dof]] = dof_sup[dof] + (subdomain_operator.num_dofs - num_interface_dofs);
            }
        }
    }

    for (int e = num_superdomain_elems; e < num_superdomain_extended_elems; e++)
    {
        auto &elem = superdomain_region[e];
        auto &slem = subdomain_region[subdomain_partition[elem.id] - 1];

        for (int v = 0; v < elem.num_points; v++)
        {
            int dof = dof_num_coarse[elem.id * num_vertices + v];

            if (dof > 0)
            {
                dof -= 1;

                if (dof_marker[dof] == 4)
                {
                    superdomain_dof_mapping[dof_sup[dof]] = slem.dof_num[v];
                }
            }
        }
    }

    Q_int.initialize(subdomain_operator.num_extended_dofs + superdomain_operator.num_extended_dofs, num_dofs);

    for (int i = 0; i < subdomain_operator.num_extended_dofs; i++)
        Q_int.add_entry(i, subdomain_dof_mapping[i + 1] - 1, 1.0);

    for (int i = 0; i < superdomain_operator.num_extended_dofs; i++)
        Q_int.add_entry(subdomain_operator.num_extended_dofs + i, superdomain_dof_mapping[i + 1] - 1, 1.0);

    Q_int.assemble();

    Qt_int.initialize(num_dofs, subdomain_operator.num_extended_dofs + superdomain_operator.num_extended_dofs);

    for (int i = 0; i < subdomain_operator.num_dofs; i++)
        Qt_int.add_entry(i, i, 1.0);

    for (int i = 0; i < superdomain_operator.num_dofs - num_interface_dofs; i++)
        Qt_int.add_entry(subdomain_operator.num_dofs + i, subdomain_operator.num_extended_dofs + num_interface_dofs + i, 1.0);

    Qt_int.assemble();

    QQt_int.initialize(subdomain_operator.num_extended_dofs + superdomain_operator.num_extended_dofs, subdomain_operator.num_extended_dofs + superdomain_operator.num_extended_dofs);

    for (int i = 0; i < subdomain_operator.num_extended_dofs + superdomain_operator.num_extended_dofs; i++) work_hst[0][i] = 0.0;

    for (int i = 0; i < subdomain_operator.num_dofs; i++)
    {
        QQt_int.add_entry(i, i, 1.0);
        work_hst[0][i] = 1.0;
    }

    for (int e = num_subdomain_elems; e < num_subdomain_extended_elems; e++)
    {
        auto &elem = subdomain_region[e];

        for (int v = 0; v < elem.num_points; v++)
        {
            if ((elem.dof_num[v] > 0) and (work_hst[0][elem.dof_num[v] - 1] == 0))
            {
                QQt_int.add_entry(elem.dof_num[v] - 1, subdomain_operator.num_extended_dofs + dof_sup[dof_num_coarse[elem.id * num_vertices + v] - 1] - 1, 1.0);
                work_hst[0][elem.dof_num[v] - 1] = 1.0;
            }
        }
    }

    for (int i = 0; i < num_interface_dofs; i++)
    {
        QQt_int.add_entry(subdomain_operator.num_extended_dofs + i, subdomain_operator.num_dofs - num_interface_dofs + i, 1.0);
        work_hst[0][subdomain_operator.num_extended_dofs + i] = 1.0;
    }

    for (int i = num_interface_dofs; i < superdomain_operator.num_dofs; i++)
    {
        QQt_int.add_entry(subdomain_operator.num_extended_dofs + i, subdomain_operator.num_extended_dofs + i, 1.0);
        work_hst[0][subdomain_operator.num_extended_dofs + i] = 1.0;
    }

    for (int e = num_superdomain_elems; e < num_superdomain_extended_elems; e++)
    {
        auto &elem = superdomain_region[e];
        auto &slem = subdomain_region[subdomain_partition[elem.id] - 1];

        for (int v = 0; v < elem.num_points; v++)
        {
            if (dof_num_coarse[elem.id * num_vertices + v] > 0)
            {
                int dof = dof_sup[dof_num_coarse[elem.id * num_vertices + v] - 1];

                if (work_hst[0][subdomain_operator.num_extended_dofs + dof - 1] == 0)
                {
                    QQt_int.add_entry(subdomain_operator.num_extended_dofs + dof - 1, slem.dof_num[v] - 1, 1.0);
                    work_hst[0][subdomain_operator.num_extended_dofs + dof - 1] = 1.0;
                }
            }
        }
    }

    QQt_int.assemble();

    // Norm weighting
    norm_weight = device.malloc<DType>(subdomain_operator.num_extended_dofs + superdomain_operator.num_extended_dofs);
    for (int i = 0; i < subdomain_operator.num_extended_dofs + superdomain_operator.num_extended_dofs; i++) work_hst[0][i] = 1.0;
    for (int i = subdomain_operator.num_dofs; i < subdomain_operator.num_extended_dofs; i++) work_hst[0][i] = 0.0;
    for (int i = 0; i < num_interface_dofs; i++) work_hst[0][subdomain_operator.num_extended_dofs + i] = 0.0;
    for (int i = superdomain_operator.num_dofs; i < superdomain_operator.num_extended_dofs; i++) work_hst[0][subdomain_operator.num_extended_dofs + i] = 0.0;
    norm_weight.copyFrom(work_hst[0].data(), (subdomain_operator.num_extended_dofs + superdomain_operator.num_extended_dofs) * sizeof(DType));

    // Inner product weight
    inner_weight = device.malloc<DType>(subdomain_operator.num_points + superdomain_operator.num_extended_dofs);
    subdomain_operator.Q.multiply(inner_weight, norm_weight);
    occa::memory inner_weight_superdomain = inner_weight.slice(subdomain_operator.num_points, superdomain_operator.num_extended_dofs);
    occa::memory norm_weight_superdomain = norm_weight.slice(subdomain_operator.num_extended_dofs, superdomain_operator.num_extended_dofs);
    inner_weight_superdomain.copyFrom(norm_weight_superdomain, superdomain_operator.num_extended_dofs * sizeof(DType));
    inner_weight.copyTo(work_hst[0].data(), (subdomain_operator.num_points + superdomain_operator.num_extended_dofs) * sizeof(DType));
    for (int i = 0; i < subdomain_operator.num_points + superdomain_operator.num_extended_dofs; i++) if (work_hst[0][i] > 0.0) work_hst[0][i] = 1.0;
    inner_weight.copyFrom(work_hst[0].data(), (subdomain_operator.num_points + superdomain_operator.num_extended_dofs) * sizeof(DType));

    // Low-order preconditioner
    rstdout("Assembling subdomain low-order preconditioner\n");

    if (use_preconditioner)
    {
        std::map<std::pair<int, int>, std::vector<DType>> J_cf_fem;

        for (int l_f = 0; l_f < num_levels - 1; l_f++)
        {
            for (int l_c = l_f + 1; l_c < num_levels; l_c++)
            {
                int N_f = poly_degree[l_f];
                int N_c = poly_degree[l_c];
                int n_f = N_f + 1;
                int n_c = N_c + 1;
                std::pair<int, int> idx(N_c, N_f);

                J_cf_fem[idx].resize(n_c * n_f);
                J_cf_fem[idx][0 * n_c + 0] = 1.0;

                for (int i = 1; i < N_f; i++)
                {
                    for (int j = 0; j < N_c; j++)
                    {
                        if ((r_gll[l_c][j] <= r_gll[l_f][i]) and (r_gll[l_f][i] <= r_gll[l_c][j + 1]))
                        {
                            J_cf_fem[idx][i * n_c + (j + 0)] = (r_gll[l_c][j + 1] - r_gll[l_f][i]) / (r_gll[l_c][j + 1] - r_gll[l_c][j]);
                            J_cf_fem[idx][i * n_c + (j + 1)] = (r_gll[l_f][i + 0] - r_gll[l_c][j]) / (r_gll[l_c][j + 1] - r_gll[l_c][j]);
                        }
                    }
                }

                J_cf_fem[idx][(n_f - 1) * n_c + (n_c - 1)] = 1.0;
            }
        }

        auto determinant = [&](const std::vector<DType> &A)
        {
            if (dim == 2)
                return A[0] * A[3] - A[1] * A[2];
            else
                return A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) + A[2] * (A[3] * A[7] - A[4] * A[6]);
        };

        auto inverse = [&](std::vector<DType> &inv_A, const std::vector<DType> &A)
        {
            DType det_A = determinant(A);

            if (dim == 2)
            {
                inv_A[0] =   (1.0 / det_A) * A[3];
                inv_A[1] = - (1.0 / det_A) * A[1];
                inv_A[2] = - (1.0 / det_A) * A[2];
                inv_A[3] =   (1.0 / det_A) * A[0];
            }
            else
            {
                inv_A[0] = (1.0 / det_A) * (A[4] * A[8] - A[7] * A[5]);
                inv_A[1] = (1.0 / det_A) * (A[2] * A[7] - A[8] * A[1]);
                inv_A[2] = (1.0 / det_A) * (A[1] * A[5] - A[4] * A[2]);
                inv_A[3] = (1.0 / det_A) * (A[5] * A[6] - A[8] * A[3]);
                inv_A[4] = (1.0 / det_A) * (A[0] * A[8] - A[6] * A[2]);
                inv_A[5] = (1.0 / det_A) * (A[2] * A[3] - A[5] * A[0]);
                inv_A[6] = (1.0 / det_A) * (A[3] * A[7] - A[6] * A[4]);
                inv_A[7] = (1.0 / det_A) * (A[1] * A[6] - A[7] * A[0]);
                inv_A[8] = (1.0 / det_A) * (A[0] * A[4] - A[3] * A[1]);
            }
        };

        int one = 1;
        int row;
        int col;
        DType val;

        int num_verts = (dim == 2) ? 3 : 4;
        int num_quads = (dim == 2) ? 3 : 4;
        DType weight = (dim == 2) ? 6.0 : 24.0;

        DType det_H_fem;
        std::vector<DType> H_fem(dim * dim);
        std::vector<DType> inv_H_fem(dim * dim);
        std::vector<std::vector<DType>> G_fem(dim * dim, std::vector<DType>(num_quads * num_quads));
        std::vector<std::vector<DType>> D_fem(dim, std::vector<DType>(num_quads * num_verts));

        if (dim == 2)
        {
            D_fem[0] = { -1.0, 1.0, 0.0, -1.0, 1.0, 0.0, -1.0, 1.0, 0.0 };
            D_fem[1] = { -1.0, 0.0, 1.0, -1.0, 0.0, 1.0, -1.0, 0.0, 1.0 };
        }
        else
        {
            D_fem[0] = { 1.0, 0.0, 0.0, -1.0, 1.0, 0.0, 0.0, -1.0, 1.0, 0.0, 0.0, -1.0, 1.0, 0.0, 0.0, -1.0 };
            D_fem[1] = { 0.0, 1.0, 0.0, -1.0, 0.0, 1.0, 0.0, -1.0, 0.0, 1.0, 0.0, -1.0, 0.0, 1.0, 0.0, -1.0 };
            D_fem[2] = { 0.0, 0.0, 1.0, -1.0, 0.0, 0.0, 1.0, -1.0, 0.0, 0.0, 1.0, -1.0, 0.0, 0.0, 1.0, -1.0 };
        }

        int num_low_order_elems = (dim == 2) ? 2 : 6;
        int loc_sub[num_verts];
        DType x_sub[num_verts];
        DType y_sub[num_verts];
        DType z_sub[num_verts];

        std::vector<std::vector<std::tuple<int, int, int>>> low_order_elems(num_low_order_elems, std::vector<std::tuple<int, int, int>>(num_verts));

        if (dim == 2) // Ignore third argument for 2D
        {
            if (num_low_order_elems == 2)
            {
                low_order_elems[0] = { std::tuple<int, int, int>(0, 0, 0), std::tuple<int, int, int>(1, 0, 0), std::tuple<int, int, int>(1, 1, 0) };
                low_order_elems[1] = { std::tuple<int, int, int>(1, 1, 0), std::tuple<int, int, int>(0, 1, 0), std::tuple<int, int, int>(0, 0, 0) };
            }
            else if (num_low_order_elems == 4)
            {
                low_order_elems[0] = { std::tuple<int, int, int>(0, 0, 0), std::tuple<int, int, int>(1, 0, 0), std::tuple<int, int, int>(1, 1, 0) };
                low_order_elems[1] = { std::tuple<int, int, int>(1, 0, 0), std::tuple<int, int, int>(1, 1, 0), std::tuple<int, int, int>(0, 1, 0) };
                low_order_elems[2] = { std::tuple<int, int, int>(1, 1, 0), std::tuple<int, int, int>(0, 1, 0), std::tuple<int, int, int>(0, 0, 0) };
                low_order_elems[3] = { std::tuple<int, int, int>(0, 1, 0), std::tuple<int, int, int>(0, 0, 0), std::tuple<int, int, int>(1, 0, 0) };
            }
            else
            {
                pstdout("Number of low order elements not supported\n");
                quit();
            }
        }
        else
        {
            if (num_low_order_elems == 6)
            {
                low_order_elems[0] = { std::tuple<int, int, int>(0, 0, 0), std::tuple<int, int, int>(0, 1, 0), std::tuple<int, int, int>(1, 0, 0), std::tuple<int, int, int>(1, 0, 1) };
                low_order_elems[1] = { std::tuple<int, int, int>(1, 0, 0), std::tuple<int, int, int>(0, 1, 0), std::tuple<int, int, int>(1, 1, 0), std::tuple<int, int, int>(1, 0, 1) };
                low_order_elems[2] = { std::tuple<int, int, int>(0, 0, 0), std::tuple<int, int, int>(0, 0, 1), std::tuple<int, int, int>(0, 1, 0), std::tuple<int, int, int>(1, 0, 1) };
                low_order_elems[3] = { std::tuple<int, int, int>(1, 0, 1), std::tuple<int, int, int>(1, 1, 0), std::tuple<int, int, int>(1, 1, 1), std::tuple<int, int, int>(0, 1, 0) };
                low_order_elems[4] = { std::tuple<int, int, int>(0, 0, 1), std::tuple<int, int, int>(1, 0, 1), std::tuple<int, int, int>(0, 1, 1), std::tuple<int, int, int>(0, 1, 0) };
                low_order_elems[5] = { std::tuple<int, int, int>(1, 0, 1), std::tuple<int, int, int>(1, 1, 1), std::tuple<int, int, int>(0, 1, 1), std::tuple<int, int, int>(0, 1, 0) };
            }
            else if (num_low_order_elems == 8)
            {
                low_order_elems[0] = { std::tuple<int, int, int>(0, 0, 0), std::tuple<int, int, int>(0, 1, 0), std::tuple<int, int, int>(1, 0, 0), std::tuple<int, int, int>(0, 0, 1) };
                low_order_elems[1] = { std::tuple<int, int, int>(1, 0, 0), std::tuple<int, int, int>(0, 0, 0), std::tuple<int, int, int>(1, 1, 0), std::tuple<int, int, int>(1, 0, 1) };
                low_order_elems[2] = { std::tuple<int, int, int>(0, 1, 0), std::tuple<int, int, int>(0, 1, 1), std::tuple<int, int, int>(1, 1, 0), std::tuple<int, int, int>(0, 0, 0) };
                low_order_elems[3] = { std::tuple<int, int, int>(1, 1, 0), std::tuple<int, int, int>(0, 1, 0), std::tuple<int, int, int>(1, 1, 1), std::tuple<int, int, int>(1, 0, 0) };
                low_order_elems[4] = { std::tuple<int, int, int>(0, 0, 1), std::tuple<int, int, int>(1, 0, 1), std::tuple<int, int, int>(0, 1, 1), std::tuple<int, int, int>(0, 0, 0) };
                low_order_elems[5] = { std::tuple<int, int, int>(1, 0, 1), std::tuple<int, int, int>(1, 1, 1), std::tuple<int, int, int>(0, 0, 1), std::tuple<int, int, int>(1, 0, 0) };
                low_order_elems[6] = { std::tuple<int, int, int>(0, 1, 1), std::tuple<int, int, int>(1, 1, 1), std::tuple<int, int, int>(0, 1, 0), std::tuple<int, int, int>(0, 0, 1) };
                low_order_elems[7] = { std::tuple<int, int, int>(1, 1, 1), std::tuple<int, int, int>(1, 1, 0), std::tuple<int, int, int>(0, 1, 1), std::tuple<int, int, int>(1, 0, 1) };
            }
            else
            {
                pstdout("Number of low order elements not supported\n");
                quit();
            }
        }

        for (int g = 0; g < NUM_GEOM_FACTS; g++)
        {
            int i = 0;

            subdomain_operator.geom_fact[g].copyTo(work_hst[0].data(), subdomain_operator.num_points * sizeof(DType));

            for (auto &elem : subdomain_region)
                for (int v = 0; v < elem.num_points; v++)
                    elem.geom_fact[g][v] = work_hst[0][i++];
        }

        HYPRE_ParCSRMatrix A_sub_fem_csr;
        HYPRE_IJMatrix A_sub_fem;
        HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, subdomain_operator.num_extended_dofs - 1, 0, subdomain_operator.num_extended_dofs - 1, &A_sub_fem);
        HYPRE_IJMatrixSetObjectType(A_sub_fem, HYPRE_PARCSR);
        HYPRE_IJMatrixInitialize_v2(A_sub_fem, HYPRE_MEMORY_HOST);

        for (int e = 0; e < (int)(subdomain_region.size()); e++)
        {
            auto &elem_i = subdomain_region[e];

            int N_i = elem_i.poly_degree;
            int n_i = N_i + 1;

            HYPRE_ParCSRMatrix A_e_csr;
            HYPRE_IJMatrix A_e;
            HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, elem_i.num_points - 1, 0, elem_i.num_points - 1, &A_e);
            HYPRE_IJMatrixSetObjectType(A_e, HYPRE_PARCSR);
            HYPRE_IJMatrixInitialize_v2(A_e, HYPRE_MEMORY_HOST);

            if (N_i > 1)
            {
                int S_x = (dim >= 1) ? elem_i.poly_degree : 1;
                int S_y = (dim >= 2) ? elem_i.poly_degree : 1;
                int S_z = (dim >= 3) ? elem_i.poly_degree : 1;

                for (int s_z = 0; s_z < S_z; s_z++)
                {
                    for (int s_y = 0; s_y < S_y; s_y++)
                    {
                        for (int s_x = 0; s_x < S_x; s_x++)
                        {
                            for (auto &low_order_elem : low_order_elems)
                            {
                                for (int vid = 0; vid < num_verts; vid++)
                                {
                                    int i = std::get<0>(low_order_elem[vid]);
                                    int j = std::get<1>(low_order_elem[vid]);
                                    int k = std::get<2>(low_order_elem[vid]);

                                    if (dim == 2)
                                        loc_sub[vid] = (s_x + i) + (s_y + j) * n_i;
                                    else
                                        loc_sub[vid] = (s_x + i) + (s_y + j) * n_i + (s_z + k) * (n_i * n_i);

                                    if (dim >= 1) x_sub[vid] = elem_i.x[loc_sub[vid]];
                                    if (dim >= 2) y_sub[vid] = elem_i.y[loc_sub[vid]];
                                    if (dim >= 3) z_sub[vid] = elem_i.z[loc_sub[vid]];
                                }

                                if (dim == 2)
                                {
                                    H_fem[0] = x_sub[1] - x_sub[0];
                                    H_fem[1] = x_sub[2] - x_sub[0];
                                    H_fem[2] = y_sub[1] - y_sub[0];
                                    H_fem[3] = y_sub[2] - y_sub[0];
                                }
                                else
                                {
                                    H_fem[0] = x_sub[0] - x_sub[3];
                                    H_fem[1] = x_sub[1] - x_sub[3];
                                    H_fem[2] = x_sub[2] - x_sub[3];
                                    H_fem[3] = y_sub[0] - y_sub[3];
                                    H_fem[4] = y_sub[1] - y_sub[3];
                                    H_fem[5] = y_sub[2] - y_sub[3];
                                    H_fem[6] = z_sub[0] - z_sub[3];
                                    H_fem[7] = z_sub[1] - z_sub[3];
                                    H_fem[8] = z_sub[2] - z_sub[3];
                                }

                                inverse(inv_H_fem, H_fem);
                                det_H_fem = determinant(H_fem);

                                for (int i = 0; i < num_quads; i++)
                                {
                                    for (int m = 0; m < dim; m++)
                                    {
                                        for (int n = 0; n < dim; n++)
                                        {
                                            DType G_val = 0.0;

                                            for (int k = 0; k < dim; k++)
                                                G_val += (det_H_fem / weight) * inv_H_fem[m * dim + k] * inv_H_fem[n * dim + k];

                                            G_fem[n + m * dim][i * num_quads + i] = G_val;
                                        }
                                    }
                                }

                                work_hst[1].assign(num_verts * num_verts, 0.0);

                                for (int m = 0; m < dim; m++)
                                {
                                    for (int n = 0; n < dim; n++)
                                    {
                                        work_hst[0].assign(num_quads * num_verts, 0.0);

                                        for (int i = 0; i < num_quads; i++)
                                            for (int j = 0; j < num_verts; j++)
                                                for (int k = 0; k < num_quads; k++)
                                                    work_hst[0][i * num_verts + j] += G_fem[n + m * dim][i * num_quads + k] * D_fem[n][k * num_verts + j];

                                        for (int i = 0; i < num_verts; i++)
                                            for (int j = 0; j < num_verts; j++)
                                                for (int k = 0; k < num_quads; k++)
                                                    work_hst[1][i * num_verts + j] += D_fem[m][k * num_verts + i] * work_hst[0][k * num_verts + j];
                                    }
                                }

                                for (int i = 0; i < num_verts; i++)
                                {
                                    for (int j = 0; j < num_verts; j++)
                                    {
                                        if (std::abs(work_hst[1][i * num_verts + j]) > epsilon)
                                        {
                                            row = loc_sub[i];
                                            col = loc_sub[j];
                                            val = work_hst[1][i * num_verts + j];

                                            HYPRE_IJMatrixAddToValues(A_e, 1, &one, &row, &col, &val);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                if (dim == 2)
                {
                    for (int g = 0; g < NUM_GEOM_FACTS; g++)
                        for (int v = 0; v < 4; v++)
                            G[g][v * 4 + v] = elem_i.geom_fact[g][v];

                    for (int i = 0; i < 4; i++)
                    {
                        for (int j = 0; j < 4; j++)
                        {
                            DType GD_1 = 0.0;
                            DType GD_2 = 0.0;

                            for (int k = 0; k < 4; k++)
                            {
                                GD_1 += G[0][i * 4 + k] * D[0][k * 4 + j] + G[2][i * 4 + k] * D[1][k * 4 + j];
                                GD_2 += G[2][i * 4 + k] * D[0][k * 4 + j] + G[1][i * 4 + k] * D[1][k * 4 + j];
                            }

                            GD[0][i * 4 + j] = GD_1;
                            GD[1][i * 4 + j] = GD_2;
                        }
                    }

                    for (int i = 0; i < 4; i++)
                    {
                        for (int j = 0; j < 4; j++)
                        {
                            row = i;
                            col = j;
                            val = 0.0;

                            for (int k = 0; k < 4; k++)
                                val += D[0][k * 4 + i] * GD[0][k * 4 + j] + D[1][k * 4 + i] * GD[1][k * 4 + j];

                            if (std::abs(val) > epsilon)
                                HYPRE_IJMatrixAddToValues(A_e, 1, &one, &row, &col, &val);
                        }
                    }
                }
                else
                {
                    for (int g = 0; g < NUM_GEOM_FACTS; g++)
                        for (int v = 0; v < 8; v++)
                            G[g][v * 8 + v] = elem_i.geom_fact[g][v];

                    for (int i = 0; i < 8; i++)
                    {
                        for (int j = 0; j < 8; j++)
                        {
                            DType GD_1 = 0.0;
                            DType GD_2 = 0.0;
                            DType GD_3 = 0.0;

                            for (int k = 0; k < 8; k++)
                            {
                                GD_1 += G[0][i * 8 + k] * D[0][k * 8 + j] + G[3][i * 8 + k] * D[1][k * 8 + j] + G[4][i * 8 + k] * D[2][k * 8 + j];
                                GD_2 += G[3][i * 8 + k] * D[0][k * 8 + j] + G[1][i * 8 + k] * D[1][k * 8 + j] + G[5][i * 8 + k] * D[2][k * 8 + j];
                                GD_3 += G[4][i * 8 + k] * D[0][k * 8 + j] + G[5][i * 8 + k] * D[1][k * 8 + j] + G[2][i * 8 + k] * D[2][k * 8 + j];
                            }

                            GD[0][i * 8 + j] = GD_1;
                            GD[1][i * 8 + j] = GD_2;
                            GD[2][i * 8 + j] = GD_3;
                        }
                    }

                    for (int i = 0; i < 8; i++)
                    {
                        for (int j = 0; j < 8; j++)
                        {
                            row = i;
                            col = j;
                            val = 0.0;

                            for (int k = 0; k < 8; k++)
                                val += D[0][k * 8 + i] * GD[0][k * 8 + j] + D[1][k * 8 + i] * GD[1][k * 8 + j] + D[2][k * 8 + i] * GD[2][k * 8 + j];

                            if (std::abs(val) > epsilon)
                                HYPRE_IJMatrixAddToValues(A_e, 1, &one, &row, &col, &val);
                        }
                    }
                }
            }

            HYPRE_IJMatrixAssemble(A_e);
            HYPRE_IJMatrixGetObject(A_e, (void**)(&A_e_csr));

            std::vector<std::pair<int, int>> vert_conn(elem_i.num_points);
            std::vector<std::pair<std::vector<int>, std::vector<std::pair<int, int>>>> edge_conn(num_edges);
            std::vector<std::pair<std::vector<int>, std::vector<std::pair<int, int>>>> face_conn(num_faces);
            int rank = 1;

            for (int vid = 0; vid < elem_i.num_points; vid++)
            {
                if (elem_i.glo_num[vid] > 0)
                    vert_conn[vid].first = rank++;
                else
                    vert_conn[vid].first = 0;
        
                vert_conn[vid].second = elem_i.dof_num[vid];
            }

            for (int eid = 0; eid < num_edges; eid++)
            {
                int e_j = -1;
                int N_j = N_i;
                int n_j = N_j + 1;

                for (auto e : elem_i.edge_conn[eid])
                {
                    if (subdomain_region[e].poly_degree < N_j)
                    {
                        e_j = e;
                        N_j = subdomain_region[e].poly_degree;
                        n_j = N_j + 1;
                    }
                }

                if (e_j >= 0)
                {
                    auto &elem_j = subdomain_region[e_j];

                    auto match = matching_edge(elem_i, elem_j, eid);
                    std::vector<int> &idx_i = match.first;
                    std::vector<int> &idx_j = match.second;

                    edge_conn[eid].first = idx_i;

                    edge_conn[eid].second.resize(n_j);
                    edge_conn[eid].second[0] = vert_conn[idx_i[0]];
                    edge_conn[eid].second[n_j - 1] = vert_conn[idx_i[n_i - 1]];

                    for (int k = 1; k < n_j - 1; k++)
                    {
                        edge_conn[eid].second[k].first = rank++;
                        edge_conn[eid].second[k].second = elem_j.dof_num[idx_j[k]];
                    }
                }
            }

            for (int fid = 0; fid < num_faces; fid++)
            {
                for (auto e_j : elem_i.face_conn[fid])
                {
                    auto &elem_j = subdomain_region[e_j];
                    int N_j = elem_j.poly_degree;
                    int n_j = N_j + 1;

                    if (N_i > N_j)
                    {
                        auto match = matching_face(elem_i, elem_j, fid);
                        std::vector<int> &idx_i = match.first;
                        std::vector<int> &idx_j = match.second;

                        face_conn[fid].first = idx_i;

                        face_conn[fid].second.resize(n_j * n_j);
                        face_conn[fid].second[0 + 0 * n_j] = vert_conn[idx_i[0 + 0 * n_i]];
                        face_conn[fid].second[(n_j - 1) + 0 * n_j] = vert_conn[idx_i[(n_i - 1) + 0 * n_i]];
                        face_conn[fid].second[0 + (n_j - 1) * n_j] = vert_conn[idx_i[0 + (n_i - 1) * n_i]];
                        face_conn[fid].second[(n_j - 1) + (n_j - 1) * n_j] = vert_conn[idx_i[(n_i - 1) + (n_i - 1) * n_i]];

                        if (fid == 0)
                        {
                            for (int k = 1; k < n_j - 1; k++)
                            {
                                face_conn[fid].second[k + 0 * n_j] = edge_conn[0].second[k];
                                face_conn[fid].second[k + (n_j - 1) * n_j] = edge_conn[1].second[k];
                                face_conn[fid].second[0 + k * n_j] = edge_conn[2].second[k];
                                face_conn[fid].second[(n_j - 1) + k * n_j] = edge_conn[3].second[k];
                            }
                        }
                        else if (fid == 1)
                        {
                            for (int k = 1; k < n_j - 1; k++)
                            {
                                face_conn[fid].second[k + 0 * n_j] = edge_conn[4].second[k];
                                face_conn[fid].second[k + (n_j - 1) * n_j] = edge_conn[5].second[k];
                                face_conn[fid].second[0 + k * n_j] = edge_conn[6].second[k];
                                face_conn[fid].second[(n_j - 1) + k * n_j] = edge_conn[7].second[k];
                            }
                        }
                        else if (fid == 2)
                        {
                            for (int k = 1; k < n_j - 1; k++)
                            {
                                face_conn[fid].second[k + 0 * n_j] = edge_conn[0].second[k];
                                face_conn[fid].second[k + (n_j - 1) * n_j] = edge_conn[4].second[k];
                                face_conn[fid].second[0 + k * n_j] = edge_conn[8].second[k];
                                face_conn[fid].second[(n_j - 1) + k * n_j] = edge_conn[9].second[k];
                            }
                        }
                        else if (fid == 3)
                        {
                            for (int k = 1; k < n_j - 1; k++)
                            {
                                face_conn[fid].second[k + 0 * n_j] = edge_conn[1].second[k];
                                face_conn[fid].second[k + (n_j - 1) * n_j] = edge_conn[5].second[k];
                                face_conn[fid].second[0 + k * n_j] = edge_conn[10].second[k];
                                face_conn[fid].second[(n_j - 1) + k * n_j] = edge_conn[11].second[k];
                            }
                        }
                        else if (fid == 4)
                        {
                            for (int k = 1; k < n_j - 1; k++)
                            {
                                face_conn[fid].second[k + 0 * n_j] = edge_conn[2].second[k];
                                face_conn[fid].second[k + (n_j - 1) * n_j] = edge_conn[6].second[k];
                                face_conn[fid].second[0 + k * n_j] = edge_conn[8].second[k];
                                face_conn[fid].second[(n_j - 1) + k * n_j] = edge_conn[10].second[k];
                            }
                        }
                        else if (fid == 5)
                        {
                            for (int k = 1; k < n_j - 1; k++)
                            {
                                face_conn[fid].second[k + 0 * n_j] = edge_conn[3].second[k];
                                face_conn[fid].second[k + (n_j - 1) * n_j] = edge_conn[7].second[k];
                                face_conn[fid].second[0 + k * n_j] = edge_conn[9].second[k];
                                face_conn[fid].second[(n_j - 1) + k * n_j] = edge_conn[11].second[k];
                            }
                        }

                        for (int j = 1; j < n_j - 1; j++)
                        {
                            for (int i = 1; i < n_j - 1; i++)
                            {
                                face_conn[fid].second[i + j * n_j].first = rank++;
                                face_conn[fid].second[i + j * n_j].second = elem_j.dof_num[idx_j[i + j * n_j]];
                            }
                        }
                    }
                }
            }

            int num_rows = elem_i.num_points;
            int num_cols = rank - 1;

            HYPRE_ParCSRMatrix J_e_csr;
            HYPRE_IJMatrix J_e;
            HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, num_rows - 1, 0, num_cols - 1, &J_e);
            HYPRE_IJMatrixSetObjectType(J_e, HYPRE_PARCSR);
            HYPRE_IJMatrixInitialize_v2(J_e, HYPRE_MEMORY_HOST);

            for (int vid = 0; vid < elem_i.num_points; vid++)
            {
                if (vert_conn[vid].first > 0)
                {
                    row = vid;
                    col = vert_conn[vid].first - 1;
                    val = 1.0;

                    HYPRE_IJMatrixAddToValues(J_e, 1, &one, &row, &col, &val);
                }
            }

            for (int eid = 0; eid < num_edges; eid++)
            {
                if (edge_conn[eid].second.size() > 0)
                {
                    int N_j = edge_conn[eid].second.size() - 1;
                    int n_j = N_j + 1;

                    std::vector<DType> &J_cf_e = J_cf_fem[std::pair<int, int>(N_j, N_i)];
                    std::vector<int> &idx_i = edge_conn[eid].first;
                    std::vector<std::pair<int, int>> &idx_j = edge_conn[eid].second;

                    for (int i = 1; i < n_i - 1; i++)
                    {
                        for (int j = 0; j < n_j; j++)
                        {
                            row = idx_i[i];
                            col = idx_j[j].first - 1;
                            val = J_cf_e[i * n_j + j];

                            if (std::abs(val) > epsilon)
                                HYPRE_IJMatrixAddToValues(J_e, 1, &one, &row, &col, &val);
                        }
                    }
                }
            }

            for (int fid = 0; fid < num_faces; fid++)
            {
                if (face_conn[fid].second.size() > 0)
                {
                    int N_j = std::sqrt(face_conn[fid].second.size()) - 1;
                    int n_j = N_j + 1;

                    std::vector<DType> &J_cf_e = J_cf_fem[std::pair<int, int>(N_j, N_i)];
                    std::vector<int> &idx_i = face_conn[fid].first;
                    std::vector<std::pair<int, int>> &idx_j = face_conn[fid].second;

                    for (int j = 1; j < n_i - 1; j++)
                    {
                        for (int i = 1; i < n_i - 1; i++)
                        {
                            for (int q = 0; q < n_j; q++)
                            {
                                for (int p = 0; p < n_j; p++)
                                {
                                    row = idx_i[i + j * n_i];
                                    col = idx_j[p + q * n_j].first - 1;
                                    val = J_cf_e[i * n_j + p] * J_cf_e[j * n_j + q];

                                    if (std::abs(val) > epsilon)
                                        HYPRE_IJMatrixAddToValues(J_e, 1, &one, &row, &col, &val);
                                }
                            }
                        }
                    }
                }
            }

            HYPRE_IJMatrixAssemble(J_e);
            HYPRE_IJMatrixGetObject(J_e, (void**)(&J_e_csr));

            HYPRE_ParCSRMatrix JtA_e_csr = hypre_ParCSRTMatMatKTHost(J_e_csr, A_e_csr, 0);
            HYPRE_ParCSRMatrix JtAJ_e_csr = hypre_ParCSRMatMatHost(JtA_e_csr, J_e_csr);

            std::vector<int> dof_num(num_cols);

            for (int vid = 0; vid < elem_i.num_points; vid++)
                if (vert_conn[vid].first > 0)
                    dof_num[vert_conn[vid].first - 1] = vert_conn[vid].second;

            for (int eid = 0; eid < num_edges; eid++)
                for (auto &pair : edge_conn[eid].second)
                    dof_num[pair.first - 1] = pair.second;

            for (int fid = 0; fid < num_faces; fid++)
                for (auto &pair : face_conn[fid].second)
                    dof_num[pair.first - 1] = pair.second;

            num_rows = hypre_ParCSRMatrixGlobalNumRows(JtAJ_e_csr);
            num_cols = hypre_ParCSRMatrixGlobalNumCols(JtAJ_e_csr);

            hypre_CSRMatrix *diag = hypre_ParCSRMatrixDiag(JtAJ_e_csr);
            HYPRE_Int *diag_i = hypre_CSRMatrixI(diag);
            HYPRE_Int *diag_j = hypre_CSRMatrixJ(diag);
            HYPRE_Complex *diag_data = hypre_CSRMatrixData(diag);

            for (int i = 0; i < num_rows; i++)
            {
                for (int ptr = diag_i[i]; ptr < diag_i[i + 1]; ptr++)
                {
                    int j = diag_j[ptr];

                    row = dof_num[i];
                    col = dof_num[j];
                    val = diag_data[ptr];

                    if ((row > 0) and (col > 0) and (std::abs(val) > epsilon))
                    {
                        row--;
                        col--;

                        HYPRE_IJMatrixAddToValues(A_sub_fem, 1, &one, &row, &col, &val);
                    }
                }
            }

            HYPRE_IJMatrixDestroy(A_e);
            HYPRE_IJMatrixDestroy(J_e);
            HYPRE_ParCSRMatrixDestroy(JtA_e_csr);
            HYPRE_ParCSRMatrixDestroy(JtAJ_e_csr);
        }

        HYPRE_IJMatrixAssemble(A_sub_fem);
        HYPRE_IJMatrixGetObject(A_sub_fem, (void**)(&A_sub_fem_csr));

        for (int i = 0; i < num_dofs; i++) work_hst[0][i] = (DType)(i);
        work_dev[0].copyFrom(work_hst[0].data(), num_dofs * sizeof(DType));
        Q_int.multiply(work_dev[1], work_dev[0]);
        work_dev[1].copyTo(work_hst[0].data(), (subdomain_operator.num_extended_dofs + superdomain_operator.num_extended_dofs) * sizeof(DType));

        // Assembled combined operator
        HYPRE_IJMatrixCreate(MPI_COMM_SELF, 0, num_dofs - 1, 0, num_dofs - 1, &A_fem_hst);
        HYPRE_IJMatrixSetObjectType(A_fem_hst, HYPRE_PARCSR);
        HYPRE_IJMatrixInitialize_v2(A_fem_hst, HYPRE_MEMORY_HOST);

        {
            HYPRE_Int *A_sub_fem_ptr = hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(A_sub_fem_csr));
            HYPRE_Int *A_sub_fem_col = hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(A_sub_fem_csr));
            HYPRE_Complex *A_sub_fem_val = hypre_CSRMatrixData(hypre_ParCSRMatrixDiag(A_sub_fem_csr));

            for (int i = 0; i < subdomain_operator.num_dofs; i++)
            {
                for (int ptr = A_sub_fem_ptr[i]; ptr < A_sub_fem_ptr[i + 1]; ptr++)
                {
                    int j = A_sub_fem_col[ptr];

                    row = (int)(work_hst[0][i]);
                    col = (int)(work_hst[0][j]);
                    val = A_sub_fem_val[ptr];

                    HYPRE_IJMatrixAddToValues(A_fem_hst, 1, &one, &row, &col, &val);
                }
            }
        }

        if (superdomain_operator.num_dofs > 0)
        {
            auto &A_sup = superdomain_operator.A;

            std::vector<int> A_sup_ptr(A_sup.num_rows + 1);
            std::vector<int> A_sup_col(A_sup.num_nnz);
            std::vector<DType> A_sup_val(A_sup.num_nnz);

            A_sup.ptr.copyTo(A_sup_ptr.data(), (A_sup.num_rows + 1) * sizeof(int));
            A_sup.col.copyTo(A_sup_col.data(), A_sup.num_nnz * sizeof(int));
            A_sup.val.copyTo(A_sup_val.data(), A_sup.num_nnz * sizeof(DType));

            for (int i = num_interface_dofs; i < superdomain_operator.num_dofs; i++)
            {
                for (int ptr = A_sup_ptr[i]; ptr < A_sup_ptr[i + 1]; ptr++)
                {
                    int j = A_sup_col[ptr];

                    row = (int)(work_hst[0][subdomain_operator.num_extended_dofs + i]);
                    col = (int)(work_hst[0][subdomain_operator.num_extended_dofs + j]);
                    val = A_sup_val[ptr];

                    HYPRE_IJMatrixAddToValues(A_fem_hst, 1, &one, &row, &col, &val);
                }
            }
        }

        HYPRE_IJMatrixAssemble(A_fem_hst);
        HYPRE_IJMatrixGetObject(A_fem_hst, (void**)(&A_fem_hst_csr));

        // AMG preconditioner
        cudaStreamCreate(&cuda_stream);

        if (cheby_order < 1) cheby_order = 1;
        if (cheby_order > 4) cheby_order = 4;

        int relax_type = 16;

        HYPRE_Solver amg_solver;
        HYPRE_BoomerAMGCreate(&amg_solver);
        HYPRE_BoomerAMGSetRelaxType(amg_solver, relax_type);
        HYPRE_BoomerAMGSetChebyOrder(amg_solver, cheby_order);
        HYPRE_BoomerAMGSetMaxIter(amg_solver, num_vcycles);
        HYPRE_BoomerAMGSetTol(amg_solver, tolerance);
        HYPRE_BoomerAMGSetPrintLevel(amg_solver, 0);
        HYPRE_BoomerAMGSetup(amg_solver, A_fem_hst_csr, NULL, NULL);

        amg_data = (hypre_ParAMGData*)(amg_solver);

        num_levels_fem = hypre_ParAMGDataNumLevels(amg_data);
        hypre_ParCSRMatrix **A_hyp = hypre_ParAMGDataAArray(amg_data);
        hypre_ParCSRMatrix **R_hyp = hypre_ParAMGDataRArray(amg_data);
        HYPRE_Real **coefs_hyp = hypre_ParAMGDataChebyCoefs(amg_data);
        hypre_Vector **ds_hyp = hypre_ParAMGDataChebyDS(amg_data);

        level_cutoff = std::max(0, std::min(num_levels_fem - 2, level_cutoff));

        A_fem.resize(num_levels_fem);
        D_val_fem.resize(num_levels_fem);
        coefs_fem.resize(num_levels_fem);
        P_fem.resize(num_levels_fem - 1);
        R_fem.resize(num_levels_fem - 1);

        for (int l = 0; l < num_levels_fem; l++)
        {
            const char *mem_loc = (l <= level_cutoff) ? "device" : "host";

            A_fem[l].initialize(mem_loc, 
                                hypre_CSRMatrixNumRows(hypre_ParCSRMatrixDiag(A_hyp[l])), 
                                hypre_CSRMatrixNumCols(hypre_ParCSRMatrixDiag(A_hyp[l])), 
                                hypre_CSRMatrixNumNonzeros(hypre_ParCSRMatrixDiag(A_hyp[l])), 
                                hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(A_hyp[l])), 
                                hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(A_hyp[l])), 
                                hypre_CSRMatrixData(hypre_ParCSRMatrixDiag(A_hyp[l])), 
                                cuda_stream);


            D_val_fem[l].initialize(mem_loc, A_fem[l].num_rows, hypre_VectorData(ds_hyp[l]), cuda_stream);
            coefs_fem[l].initialize("host", cheby_order, coefs_hyp[l]);

            if (l < num_levels_fem - 1)
            {
                P_fem[l].initialize(mem_loc, 
                                    hypre_CSRMatrixNumRows(hypre_ParCSRMatrixDiag(R_hyp[l])), 
                                    hypre_CSRMatrixNumCols(hypre_ParCSRMatrixDiag(R_hyp[l])), 
                                    hypre_CSRMatrixNumNonzeros(hypre_ParCSRMatrixDiag(R_hyp[l])), 
                                    hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(R_hyp[l])), 
                                    hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(R_hyp[l])), 
                                    hypre_CSRMatrixData(hypre_ParCSRMatrixDiag(R_hyp[l])), 
                                    cuda_stream);

                HYPRE_ParCSRMatrix Rt_hyp_l;
                hypre_ParCSRMatrixTranspose(R_hyp[l], &Rt_hyp_l, 1);

                R_fem[l].initialize(mem_loc, 
                                    hypre_CSRMatrixNumRows(hypre_ParCSRMatrixDiag(Rt_hyp_l)), 
                                    hypre_CSRMatrixNumCols(hypre_ParCSRMatrixDiag(Rt_hyp_l)), 
                                    hypre_CSRMatrixNumNonzeros(hypre_ParCSRMatrixDiag(Rt_hyp_l)), 
                                    hypre_CSRMatrixI(hypre_ParCSRMatrixDiag(Rt_hyp_l)), 
                                    hypre_CSRMatrixJ(hypre_ParCSRMatrixDiag(Rt_hyp_l)), 
                                    hypre_CSRMatrixData(hypre_ParCSRMatrixDiag(Rt_hyp_l)), 
                                    cuda_stream);

                hypre_ParCSRMatrixDestroy(Rt_hyp_l);
            }
        }

        work_hst_fem.resize(num_levels_fem);
        work_dev_fem.resize(num_levels_fem);

        for (int l = 0; l < num_levels_fem; l++)
        {
            work_hst_fem[l].initialize("host", A_fem[l].num_rows, NULL);
            work_dev_fem[l].initialize("device", A_fem[l].num_rows, NULL, cuda_stream);
        }

        f_fem.resize(num_levels_fem);

        for (int l = 0; l < num_levels_fem; l++)
        {
            f_fem[l].initialize(A_fem[l].mem_loc, A_fem[l].num_rows, NULL, cuda_stream);
            f_fem[l].set_to_value(0.0);
        }

        u_fem.resize(num_levels_fem);

        for (int l = 0; l < num_levels_fem; l++)
        {
            u_fem[l].initialize(A_fem[l].mem_loc, A_fem[l].num_rows, NULL, cuda_stream);
            u_fem[l].set_to_value(0.0);
        }

        for (int l = 0; l < num_levels_fem; l++)
        {
            Float alpha = 1.0;
            Float beta  = 0.0;

            if (strcmp(A_fem[l].mem_loc, "device") == 0)
            {
                cusparseSpMV_bufferSize(A_fem[l].cusparse_handle, CUSPARSE_OPERATION_NON_TRANSPOSE, 
                                        &alpha, A_fem[l].desc, u_fem[l].desc, &beta, f_fem[l].desc, 
                                        (typeid(Float) == typeid(double)) ? CUDA_R_64F : CUDA_R_32F, 
#if HOSTNAME == 0
                                        CUSPARSE_SPMV_CSR_ALG1, 
#else
                                        CUSPARSE_CSRMV_ALG1, 
#endif
                                        &A_fem[l].buffer_size);

                cudaMalloc((void**)(&A_fem[l].buffer_data), A_fem[l].buffer_size * sizeof(size_t));
            }

            if (l < num_levels_fem - 1)
            {
                if (strcmp(P_fem[l].mem_loc, "device") == 0)
                {
                    cusparseSpMV_bufferSize(P_fem[l].cusparse_handle, CUSPARSE_OPERATION_NON_TRANSPOSE, 
                                            &alpha, P_fem[l].desc, work_dev_fem[l + 1].desc, &beta, work_dev_fem[l].desc, 
                                            (typeid(Float) == typeid(double)) ? CUDA_R_64F : CUDA_R_32F, 
#if HOSTNAME == 0
                                            CUSPARSE_SPMV_CSR_ALG1, 
#else
                                            CUSPARSE_CSRMV_ALG1, 
#endif
                                            &P_fem[l].buffer_size);

                    cudaMalloc((void**)(&P_fem[l].buffer_data), P_fem[l].buffer_size * sizeof(size_t));
                }

                if (strcmp(R_fem[l].mem_loc, "device") == 0)
                {
                    cusparseSpMV_bufferSize(R_fem[l].cusparse_handle, CUSPARSE_OPERATION_NON_TRANSPOSE, 
                                            &alpha, R_fem[l].desc, work_dev_fem[l].desc, &beta, work_dev_fem[l + 1].desc, 
                                            (typeid(Float) == typeid(double)) ? CUDA_R_64F : CUDA_R_32F, 
#if HOSTNAME == 0
                                            CUSPARSE_SPMV_CSR_ALG1, 
#else
                                            CUSPARSE_CSRMV_ALG1, 
#endif
                                            &R_fem[l].buffer_size);

                    cudaMalloc((void**)(&R_fem[l].buffer_data), R_fem[l].buffer_size * sizeof(size_t));
                }
            }
        }

        r_fem.resize(num_levels_fem);

        for (int l = 0; l < num_levels_fem; l++)
            r_fem[l].initialize(A_fem[l].mem_loc, A_fem[l].num_rows, NULL, cuda_stream);

        v_fem.resize(num_levels_fem);
        w_fem.resize(num_levels_fem);

        for (int l = 0; l < num_levels_fem; l++)
        {
            v_fem[l].initialize(A_fem[l].mem_loc, A_fem[l].num_rows, NULL, cuda_stream);
            w_fem[l].initialize(A_fem[l].mem_loc, A_fem[l].num_rows, NULL, cuda_stream);
        }

#if USE_CUDA_GRAPH == 1
        cudaStreamBeginCapture(cuda_stream, cudaStreamCaptureModeGlobal);

        for (int l = 0; l <= level_cutoff; l++)
        {
            // Smooth solution
            if (l > 0) u_fem[l].set_to_value(0.0);

            scaled_residual(r_fem[l], w_fem[l], A_fem[l], u_fem[l], f_fem[l], D_val_fem[l], coefs_fem[l].data[cheby_order - 1], work_dev_fem[l]);

            for (int p = cheby_order - 2; p >= 0; p--)
                polynomial_evaluation(w_fem[l], v_fem[l], A_fem[l], r_fem[l], D_val_fem[l], coefs_fem[l].data[p], work_dev_fem[l]);

            update_field(u_fem[l], w_fem[l], D_val_fem[l]);

            // Compute residual
            v_fem[l].copy_from(f_fem[l]);
            A_fem[l].matvec(v_fem[l], u_fem[l], - 1.0, 1.0);

            // Restrict
            if (l == level_cutoff)
            {
                R_fem[l].matvec(work_dev_fem[l + 1], v_fem[l]);
                f_fem[l + 1].copy_from(work_dev_fem[l + 1]);
            }
            else
            {
                R_fem[l].matvec(f_fem[l + 1], v_fem[l]);
            }
        }

        cudaStreamEndCapture(cuda_stream, &down_leg_graph);
        cudaGraphInstantiate(&down_leg_instance, down_leg_graph, NULL, NULL, 0);

        cudaStreamBeginCapture(cuda_stream, cudaStreamCaptureModeGlobal);

        for (int l = level_cutoff + 1; l > 0; l--)
        {
            // Coarse grid correction
            if (l - 1 == level_cutoff)
            {
                work_dev_fem[l].copy_from(u_fem[l]);
                P_fem[l - 1].matvec(u_fem[l - 1], work_dev_fem[l], 1.0, 1.0);
            }
            else
            {
                P_fem[l - 1].matvec(u_fem[l - 1], u_fem[l], 1.0, 1.0);
            }

            // Smooth solution
            scaled_residual(r_fem[l - 1], w_fem[l - 1], A_fem[l - 1], u_fem[l - 1], f_fem[l - 1], D_val_fem[l - 1], coefs_fem[l - 1].data[cheby_order - 1], work_dev_fem[l - 1]);

            for (int p = cheby_order - 2; p >= 0; p--)
                polynomial_evaluation(w_fem[l - 1], v_fem[l - 1], A_fem[l - 1], r_fem[l - 1], D_val_fem[l - 1], coefs_fem[l - 1].data[p], work_dev_fem[l - 1]);

            update_field(u_fem[l - 1], w_fem[l - 1], D_val_fem[l - 1]);
        }

        cudaStreamEndCapture(cuda_stream, &up_leg_graph);
        cudaGraphInstantiate(&up_leg_instance, up_leg_graph, NULL, NULL, 0);
#endif
    }

#if 0
    // Testing
    {
        amg::Vector u_star_fem;

        srand(0);
        std::vector<DType> tmp(A_fem[0].num_cols);
        for (unsigned int i = 0; i < tmp.size(); i++) tmp[i] = (DType)(rand()) / (DType)(RAND_MAX);
        u_star_fem.initialize(A_fem[0].mem_loc, tmp.size(), tmp.data(), cuda_stream);

        A_fem[0].matvec(f_fem[0], u_star_fem);

        Timer<double> timer;
        timer.initialize();
        timer.start("gpu");

        num_vcycles = 100;

        Float r_0_norm = f_fem[0].norm();
        Float r_k_norm = r_0_norm;

        u_fem[0].set_to_value(0.0);

        pstdout("Iter %3d: | residual_norm = %24.16g | relative_residual_norm = %24.16g | \n", 0, r_0_norm, 1.0);

        for (int iter = 0; iter < num_vcycles; iter++)
        {
            // Down leg
#if USE_CUDA_GRAPH == 1
            cudaGraphLaunch(down_leg_instance, cuda_stream);
#else
            for (int l = 0; l <= level_cutoff; l++)
            {
                // Smooth solution
                if (l > 0) u_fem[l].set_to_value(0.0);

                scaled_residual(r_fem[l], w_fem[l], A_fem[l], u_fem[l], f_fem[l], D_val_fem[l], coefs_fem[l].data[cheby_order - 1], work_dev_fem[l]);

                for (int p = cheby_order - 2; p >= 0; p--)
                    polynomial_evaluation(w_fem[l], v_fem[l], A_fem[l], r_fem[l], D_val_fem[l], coefs_fem[l].data[p], work_dev_fem[l]);

                update_field(u_fem[l], w_fem[l], D_val_fem[l]);

                // Compute residual
                v_fem[l].copy_from(f_fem[l]);
                A_fem[l].matvec(v_fem[l], u_fem[l], - 1.0, 1.0);

                // Restrict
                if (l == level_cutoff)
                {
                    R_fem[l].matvec(work_dev_fem[l + 1], v_fem[l]);
                    f_fem[l + 1].copy_from(work_dev_fem[l + 1]);
                }
                else
                {
                    R_fem[l].matvec(f_fem[l + 1], v_fem[l]);
                }
            }
#endif

            for (int l = level_cutoff + 1; l < num_levels_fem - 1; l++)
            {
                // Smooth solution
                u_fem[l].set_to_value(0.0);

                scaled_residual(r_fem[l], w_fem[l], A_fem[l], u_fem[l], f_fem[l], D_val_fem[l], coefs_fem[l].data[cheby_order - 1], work_dev_fem[l]);

                for (int p = cheby_order - 2; p >= 0; p--)
                    polynomial_evaluation(w_fem[l], v_fem[l], A_fem[l], r_fem[l], D_val_fem[l], coefs_fem[l].data[p], work_dev_fem[l]);

                update_field(u_fem[l], w_fem[l], D_val_fem[l]);

                // Compute residual
                v_fem[l].copy_from(f_fem[l]);
                A_fem[l].matvec(v_fem[l], u_fem[l], - 1.0, 1.0);

                // Restrict
                R_fem[l].matvec(f_fem[l + 1], v_fem[l]);
            }

            // Coarse grid lolve
            memcpy(hypre_VectorData(hypre_ParVectorLocalVector(hypre_ParAMGDataFArray(amg_data)[num_levels_fem - 1])), 
                   f_fem[num_levels_fem - 1].data, 
                   f_fem[num_levels_fem - 1].size * sizeof(Float));

            hypre_GaussElimSolve(amg_data, num_levels_fem - 1, 9);

            memcpy(u_fem[num_levels_fem - 1].data, 
                   hypre_VectorData(hypre_ParVectorLocalVector(hypre_ParAMGDataUArray(amg_data)[num_levels_fem - 1])), 
                   u_fem[num_levels_fem - 1].size * sizeof(Float));

            // Up leg
            for (int l = num_levels_fem - 1; l > level_cutoff + 1; l--)
            {
                // Coarse grid correction
                P_fem[l - 1].matvec(u_fem[l - 1], u_fem[l], 1.0, 1.0);

                // Smooth solution
                scaled_residual(r_fem[l - 1], w_fem[l - 1], A_fem[l - 1], u_fem[l - 1], f_fem[l - 1], D_val_fem[l - 1], coefs_fem[l - 1].data[cheby_order - 1], work_dev_fem[l - 1]);

                for (int p = cheby_order - 2; p >= 0; p--)
                    polynomial_evaluation(w_fem[l - 1], v_fem[l - 1], A_fem[l - 1], r_fem[l - 1], D_val_fem[l - 1], coefs_fem[l - 1].data[p], work_dev_fem[l - 1]);

                update_field(u_fem[l - 1], w_fem[l - 1], D_val_fem[l - 1]);
            }

#if USE_CUDA_GRAPH == 1
            cudaGraphLaunch(up_leg_instance, cuda_stream);
#else
            for (int l = level_cutoff + 1; l > 0; l--)
            {
                // Coarse grid correction
                if (l - 1 == level_cutoff)
                {
                    work_dev_fem[l].copy_from(u_fem[l]);
                    P_fem[l - 1].matvec(u_fem[l - 1], work_dev_fem[l], 1.0, 1.0);
                }
                else
                {
                    P_fem[l - 1].matvec(u_fem[l - 1], u_fem[l], 1.0, 1.0);
                }

                // Smooth solution
                scaled_residual(r_fem[l - 1], w_fem[l - 1], A_fem[l - 1], u_fem[l - 1], f_fem[l - 1], D_val_fem[l - 1], coefs_fem[l - 1].data[cheby_order - 1], work_dev_fem[l - 1]);

                for (int p = cheby_order - 2; p >= 0; p--)
                    polynomial_evaluation(w_fem[l - 1], v_fem[l - 1], A_fem[l - 1], r_fem[l - 1], D_val_fem[l - 1], coefs_fem[l - 1].data[p], work_dev_fem[l - 1]);

                update_field(u_fem[l - 1], w_fem[l - 1], D_val_fem[l - 1]);
            }
#endif

            r_fem[0].copy_from(f_fem[0]);
            A_fem[0].matvec(r_fem[0], u_fem[0], - 1.0, 1.0);
            r_k_norm = r_fem[0].norm();

            pstdout("Iter %3d: | residual_norm = %24.16g | relative_residual_norm = %24.16g | \n", iter + 1, r_k_norm, r_k_norm / r_0_norm);

            if (r_k_norm / r_0_norm < tolerance) break;

            num_iterations++;
        }

        timer.stop("gpu");
        pstdout("Solve time: %1.16g\n", timer.total("gpu"));
    }

    quit();
#endif

    // Solver
    num_values = subdomain_operator.num_points + superdomain_operator.num_extended_dofs;

    f = device.malloc<DType>(num_values);
    u_k = device.malloc<DType>(num_values);
    r_k = device.malloc<DType>(num_values);
    r_kp1 = device.malloc<DType>(num_values);
    q_k = device.malloc<DType>(num_values);
    z_k = device.malloc<DType>(num_values);
    p_k = device.malloc<DType>(num_values);

    V.resize(num_vectors + 1); for (int i = 0; i < num_vectors + 1; i++) V[i] = device.malloc<DType>(num_values);
    Z.resize(num_vectors); for (int i = 0; i < num_vectors; i++) Z[i] = device.malloc<DType>(num_values);
    H.resize(num_vectors); for (int i = 0; i < num_vectors; i++) H[i].resize(num_vectors);
    c_gmres.resize(num_vectors);
    s_gmres.resize(num_vectors);
    gamma.resize(num_vectors + 1);

    // Kernels
    num_blocks = (num_values + BLOCK_SIZE - 1) / BLOCK_SIZE;

    occa::properties properties;

    properties["defines/DType"] = data_type;
    properties["defines/EType"] = domain.data_type;
    properties["defines/DIM"] = dim;
    properties["defines/OCCA_TYPE"] = OCCA_TYPE;
    properties["defines/BLOCK_SIZE"] = BLOCK_SIZE;

    std::string poly_degree_str;
    poly_degree_str += "const DType poly_degree[] = { ";
    for (int l = 0; l < num_levels - 1; l++) poly_degree_str += std::to_string(poly_degree[l]) + ", ";
    poly_degree_str += std::to_string(poly_degree[num_levels - 1]) + " }";
    properties["defines/POLY_DEGREE"] = poly_degree_str;

    if (proc_id == 0)
    {
        initialize_arrays_kernel = device.buildKernel("subdomain.okl", "initialize_arrays", properties);
        stiffness_matrix_1_kernel = device.buildKernel("subdomain.okl", "stiffness_matrix_1", properties);
        stiffness_matrix_2_kernel = device.buildKernel("subdomain.okl", "stiffness_matrix_2", properties);
        inner_product_kernel = device.buildKernel("subdomain.okl", "inner_product", properties);
        weighted_inner_product_kernel = device.buildKernel("subdomain.okl", "weighted_inner_product", properties);
        projection_inner_products_kernel = device.buildKernel("subdomain.okl", "projection_inner_products", properties);
        solution_and_residual_update_kernel = device.buildKernel("subdomain.okl", "solution_and_residual_update", properties);
        search_update_inner_product_kernel = device.buildKernel("subdomain.okl", "search_update_inner_product", properties);
        residual_and_search_update_kernel = device.buildKernel("subdomain.okl", "residual_and_search_update", properties);

        copy_from_domain_data_kernel = device.buildKernel("subdomain.okl", "copy_from_domain_data", properties);
        copy_to_domain_data_kernel = device.buildKernel("subdomain.okl", "copy_to_domain_data", properties);
        if (dim >= 1) restriction_1_kernel = device.buildKernel("subdomain.okl", "restriction_1", properties);
        if (dim >= 2) restriction_2_kernel = device.buildKernel("subdomain.okl", "restriction_2", properties);
        if (dim >= 3) restriction_3_kernel = device.buildKernel("subdomain.okl", "restriction_3", properties);
    }

    MPI_Barrier(MPI_COMM_WORLD);

    if (proc_id > 0)
    {
        initialize_arrays_kernel = device.buildKernel("subdomain.okl", "initialize_arrays", properties);
        stiffness_matrix_1_kernel = device.buildKernel("subdomain.okl", "stiffness_matrix_1", properties);
        stiffness_matrix_2_kernel = device.buildKernel("subdomain.okl", "stiffness_matrix_2", properties);
        inner_product_kernel = device.buildKernel("subdomain.okl", "inner_product", properties);
        weighted_inner_product_kernel = device.buildKernel("subdomain.okl", "weighted_inner_product", properties);
        projection_inner_products_kernel = device.buildKernel("subdomain.okl", "projection_inner_products", properties);
        solution_and_residual_update_kernel = device.buildKernel("subdomain.okl", "solution_and_residual_update", properties);
        search_update_inner_product_kernel = device.buildKernel("subdomain.okl", "search_update_inner_product", properties);
        residual_and_search_update_kernel = device.buildKernel("subdomain.okl", "residual_and_search_update", properties);

        copy_from_domain_data_kernel = device.buildKernel("subdomain.okl", "copy_from_domain_data", properties);
        copy_to_domain_data_kernel = device.buildKernel("subdomain.okl", "copy_to_domain_data", properties);
        if (dim >= 1) restriction_1_kernel = device.buildKernel("subdomain.okl", "restriction_1", properties);
        if (dim >= 2) restriction_2_kernel = device.buildKernel("subdomain.okl", "restriction_2", properties);
        if (dim >= 3) restriction_3_kernel = device.buildKernel("subdomain.okl", "restriction_3", properties);
    }

    MPI_Barrier(MPI_COMM_WORLD);
}

template<typename DType>
Subdomain<DType>::~Subdomain()
{

}

// Member functions
template<typename DType>
void Subdomain<DType>::stiffness_matrix(occa::memory &Au, occa::memory &u)
{
    occa::memory u_sub_l = u.slice(0, subdomain_operator.num_points);
    occa::memory u_sup = u.slice(subdomain_operator.num_points, superdomain_operator.num_extended_dofs);

    occa::memory Au_sub_l = Au.slice(0, subdomain_operator.num_points);
    occa::memory Au_sup = Au.slice(subdomain_operator.num_points, superdomain_operator.num_extended_dofs);

    superdomain_operator.A.multiply(Au_sup, u_sup);

    stiffness_matrix_1_kernel(work_dev_ptr, u_sub_l, 
                              subdomain_operator.D_hat_ptr, 
                              subdomain_operator.offset, 
                              subdomain_operator.vertex, 
                              subdomain_operator.level, 
                              subdomain_operator.geom_fact_ptr, 
                              subdomain_operator.num_points);

    stiffness_matrix_2_kernel(Au_sub_l, work_dev_ptr, 
                              subdomain_operator.D_hat_ptr, 
                              subdomain_operator.offset, 
                              subdomain_operator.vertex, 
                              subdomain_operator.level, 
                              subdomain_operator.num_points);
}

template<typename DType>
void Subdomain<DType>::direct_stiffness_summation(occa::memory &QQtu, occa::memory &u)
{
    occa::memory u_sub_l = u.slice(0, subdomain_operator.num_points);
    occa::memory u_sup = u.slice(subdomain_operator.num_points, superdomain_operator.num_extended_dofs);

    subdomain_operator.Qt.multiply(work_dev[0], u_sub_l);

    u_sup.copyTo(work_dev[0].slice(subdomain_operator.num_extended_dofs, superdomain_operator.num_extended_dofs), superdomain_operator.num_extended_dofs * sizeof(DType));
    QQt_int.multiply(work_dev[1], work_dev[0]);

    occa::memory QQtu_sub_l = QQtu.slice(0, subdomain_operator.num_points);
    occa::memory QQtu_sup = QQtu.slice(subdomain_operator.num_points, superdomain_operator.num_extended_dofs);

    subdomain_operator.Q.multiply(QQtu_sub_l, work_dev[1]);
    QQtu_sup.copyFrom(work_dev[1].slice(subdomain_operator.num_extended_dofs, superdomain_operator.num_extended_dofs), superdomain_operator.num_extended_dofs * sizeof(DType));
}

template<typename DType>
void Subdomain<DType>::low_order_preconditioner(occa::memory &z, occa::memory &r)
{
    occa::memory r_sub_l = r.slice(0, subdomain_operator.num_points);
    occa::memory r_sup = r.slice(subdomain_operator.num_points, superdomain_operator.num_extended_dofs);

    occa::memory work_sup = work_dev[0].slice(subdomain_operator.num_extended_dofs, superdomain_operator.num_extended_dofs);

    timer.start("subdomain.preconditioner.assemble_subdomain");
    subdomain_operator.Qt.multiply(work_dev[0], r_sub_l);
    timer.stop("subdomain.preconditioner.assemble_subdomain");

    timer.start("subdomain.preconditioner.memcpy");
    work_sup.copyFrom(r_sup, superdomain_operator.num_extended_dofs * sizeof(DType));
    timer.stop("subdomain.preconditioner.memcpy");

    timer.start("subdomain.preconditioner.assemble_composite");
    Qt_int.multiply(work_dev[1], work_dev[0]);
    timer.stop("subdomain.preconditioner.assemble_composite");

    timer.start("subdomain.preconditioner.memcpy");
    cudaMemcpy(f_fem[0].data, work_dev[1].ptr(), f_fem[0].size * sizeof(Float), cudaMemcpyDeviceToDevice);
    timer.stop("subdomain.preconditioner.memcpy");

    timer.start("subdomain.preconditioner.vector_operations");
    u_fem[0].set_to_value(0.0);
    timer.stop("subdomain.preconditioner.vector_operations");

    for (int iter = 0; iter < num_vcycles; iter++)
    {
        timer.start("subdomain.preconditioner.down_leg_gpu");

        // Down leg
#if USE_CUDA_GRAPH == 1
        cudaGraphLaunch(down_leg_instance, cuda_stream);
#else
        for (int l = 0; l <= level_cutoff; l++)
        {
            // Smooth solution
            if (l > 0) u_fem[l].set_to_value(0.0);

            scaled_residual(r_fem[l], w_fem[l], A_fem[l], u_fem[l], f_fem[l], D_val_fem[l], coefs_fem[l].data[cheby_order - 1], work_dev_fem[l]);

            for (int p = cheby_order - 2; p >= 0; p--)
                polynomial_evaluation(w_fem[l], v_fem[l], A_fem[l], r_fem[l], D_val_fem[l], coefs_fem[l].data[p], work_dev_fem[l]);

            update_field(u_fem[l], w_fem[l], D_val_fem[l]);

            // Compute residual
            v_fem[l].copy_from(f_fem[l]);
            A_fem[l].matvec(v_fem[l], u_fem[l], - 1.0, 1.0);

            // Restrict
            if (l == level_cutoff)
            {
                R_fem[l].matvec(work_dev_fem[l + 1], v_fem[l]);
                f_fem[l + 1].copy_from(work_dev_fem[l + 1]);
            }
            else
            {
                R_fem[l].matvec(f_fem[l + 1], v_fem[l]);
            }
        }
#endif

        timer.stop("subdomain.preconditioner.down_leg_gpu");
        timer.start("subdomain.preconditioner.down_leg_cpu");

        for (int l = level_cutoff + 1; l < num_levels_fem - 1; l++)
        {
            // Smooth solution
            u_fem[l].set_to_value(0.0);

            scaled_residual(r_fem[l], w_fem[l], A_fem[l], u_fem[l], f_fem[l], D_val_fem[l], coefs_fem[l].data[cheby_order - 1], work_dev_fem[l]);

            for (int p = cheby_order - 2; p >= 0; p--)
                polynomial_evaluation(w_fem[l], v_fem[l], A_fem[l], r_fem[l], D_val_fem[l], coefs_fem[l].data[p], work_dev_fem[l]);

            update_field(u_fem[l], w_fem[l], D_val_fem[l]);

            // Compute residual
            v_fem[l].copy_from(f_fem[l]);
            A_fem[l].matvec(v_fem[l], u_fem[l], - 1.0, 1.0);

            // Restrict
            R_fem[l].matvec(f_fem[l + 1], v_fem[l]);
        }

        timer.stop("subdomain.preconditioner.down_leg_cpu");

        // Coarse grid lolve
        timer.start("subdomain.preconditioner.coarse_grid_solver");

        memcpy(hypre_VectorData(hypre_ParVectorLocalVector(hypre_ParAMGDataFArray(amg_data)[num_levels_fem - 1])), 
               f_fem[num_levels_fem - 1].data, 
               f_fem[num_levels_fem - 1].size * sizeof(Float));

        hypre_GaussElimSolve(amg_data, num_levels_fem - 1, 9);

        memcpy(u_fem[num_levels_fem - 1].data, 
               hypre_VectorData(hypre_ParVectorLocalVector(hypre_ParAMGDataUArray(amg_data)[num_levels_fem - 1])), 
               u_fem[num_levels_fem - 1].size * sizeof(Float));

        timer.stop("subdomain.preconditioner.coarse_grid_solver");

        // Up leg
        timer.start("subdomain.preconditioner.up_leg_cpu");

        for (int l = num_levels_fem - 1; l > level_cutoff + 1; l--)
        {
            // Coarse grid correction
            P_fem[l - 1].matvec(u_fem[l - 1], u_fem[l], 1.0, 1.0);

            // Smooth solution
            scaled_residual(r_fem[l - 1], w_fem[l - 1], A_fem[l - 1], u_fem[l - 1], f_fem[l - 1], D_val_fem[l - 1], coefs_fem[l - 1].data[cheby_order - 1], work_dev_fem[l - 1]);

            for (int p = cheby_order - 2; p >= 0; p--)
                polynomial_evaluation(w_fem[l - 1], v_fem[l - 1], A_fem[l - 1], r_fem[l - 1], D_val_fem[l - 1], coefs_fem[l - 1].data[p], work_dev_fem[l - 1]);

            update_field(u_fem[l - 1], w_fem[l - 1], D_val_fem[l - 1]);
        }

        timer.stop("subdomain.preconditioner.up_leg_cpu");
        timer.start("subdomain.preconditioner.up_leg_gpu");

#if USE_CUDA_GRAPH == 1
        cudaGraphLaunch(up_leg_instance, cuda_stream);
#else
        for (int l = level_cutoff + 1; l > 0; l--)
        {
            // Coarse grid correction
            if (l - 1 == level_cutoff)
            {
                work_dev_fem[l].copy_from(u_fem[l]);
                P_fem[l - 1].matvec(u_fem[l - 1], work_dev_fem[l], 1.0, 1.0);
            }
            else
            {
                P_fem[l - 1].matvec(u_fem[l - 1], u_fem[l], 1.0, 1.0);
            }

            // Smooth solution
            scaled_residual(r_fem[l - 1], w_fem[l - 1], A_fem[l - 1], u_fem[l - 1], f_fem[l - 1], D_val_fem[l - 1], coefs_fem[l - 1].data[cheby_order - 1], work_dev_fem[l - 1]);

            for (int p = cheby_order - 2; p >= 0; p--)
                polynomial_evaluation(w_fem[l - 1], v_fem[l - 1], A_fem[l - 1], r_fem[l - 1], D_val_fem[l - 1], coefs_fem[l - 1].data[p], work_dev_fem[l - 1]);

            update_field(u_fem[l - 1], w_fem[l - 1], D_val_fem[l - 1]);
        }
#endif

        timer.stop("subdomain.preconditioner.up_leg_gpu");
    }

    timer.start("subdomain.preconditioner.memcpy");
    cudaMemcpy(work_dev[1].ptr(), u_fem[0].data, u_fem[0].size * sizeof(Float), cudaMemcpyDeviceToDevice);
    timer.stop("subdomain.preconditioner.memcpy");

    timer.start("subdomain.preconditioner.unassemble_composite");
    Q_int.multiply(work_dev[0], work_dev[1]);
    timer.stop("subdomain.preconditioner.unassemble_composite");

    occa::memory z_sub_l = z.slice(0, subdomain_operator.num_points);
    occa::memory z_sup = z.slice(subdomain_operator.num_points, superdomain_operator.num_extended_dofs);

    timer.start("subdomain.preconditioner.unassemble_subdomain");
    subdomain_operator.Q.multiply(z_sub_l, work_dev[0]);
    timer.stop("subdomain.preconditioner.unassemble_subdomain");

    timer.start("subdomain.preconditioner.memcpy");
    z_sup.copyFrom(work_sup, superdomain_operator.num_extended_dofs * sizeof(DType));
    timer.stop("subdomain.preconditioner.memcpy");
}

template<typename DType>
void Subdomain<DType>::flexible_conjugate_gradient(occa::memory &u_l, occa::memory &f_l, bool print_history, bool use_relative)
{
    // Collect tree data
    tree_operator(r_k, f_l);

    // Initialize arrays
    timer.start("subdomain.vector_operations");
    math.set_to_value(u_k, 0.0, num_values);
    timer.stop("subdomain.vector_operations");

    // Compute initial residual
    DType r_norm;
    DType r_0_norm;

    timer.start("subdomain.residual_norm");
    residual_norm(r_0_norm, r_k);
    timer.stop("subdomain.residual_norm");

    if (print_history) pstdout("- Iter %3d: | residual_norm = %24.16g | relative_residual_norm = %24.16g | \n", 0, r_0_norm, 1.0);

    // Iterative solver
    DType alpha_k;
    DType beta_k;
    DType gamma_k;
    DType theta_k;

    timer.start("subdomain.preconditioner");

    if (use_preconditioner)
        low_order_preconditioner(z_k, r_k);
    else
        direct_stiffness_summation(z_k, r_k);

    timer.stop("subdomain.preconditioner");

    timer.start("subdomain.vector_operations");
    p_k.copyFrom(z_k, num_values * sizeof(DType));
    timer.stop("subdomain.vector_operations");

    int iter = 0;

    while (iter < max_iterations)
    {
        // Projection
        timer.start("subdomain.operator_application");
        stiffness_matrix(q_k, p_k);
        timer.stop("subdomain.operator_application");

        // Inner products
        timer.start("subdomain.inner_products");
        projection_inner_products(gamma_k, theta_k, z_k, r_k, p_k, q_k);
        timer.stop("subdomain.inner_products");

        alpha_k = gamma_k / theta_k;

        // Update solution and residual
        timer.start("subdomain.vector_operations");
        solution_and_residual_update(u_k, r_kp1, r_k, p_k, q_k, alpha_k);
        timer.stop("subdomain.vector_operations");

        // Residual norm
        timer.start("subdomain.residual_norm");
        residual_norm(r_norm, r_kp1);
        timer.stop("subdomain.residual_norm");

        iter++;

        if (print_history) pstdout("- Iter %3d: | residual_norm = %24.16g | relative_residual_norm = %24.16g | \n", iter, r_norm, r_norm / r_0_norm);

        if (use_relative)
        {
            if (r_norm / r_0_norm < tolerance) break;
        }
        else
        {
            if (r_norm < tolerance) break;
        }

        if (iter == max_iterations) break;

        // Update search direction
        timer.start("subdomain.preconditioner");

        if (use_preconditioner)
            low_order_preconditioner(z_k, r_kp1);
        else
            direct_stiffness_summation(z_k, r_kp1);

        timer.stop("subdomain.preconditioner");

        timer.start("subdomain.inner_products");
        search_update_inner_product(theta_k, r_k, r_kp1, z_k);
        timer.stop("subdomain.inner_products");

        beta_k = theta_k / gamma_k;

        timer.start("subdomain.vector_operations");
        residual_and_search_update(p_k, r_k, z_k, r_kp1, beta_k);
        timer.stop("subdomain.vector_operations");
    }

    num_iterations += iter;

    timer.start("subdomain.vector_operations");
    copy_to_domain_data_kernel(u_l, u_k, levels[0].num_points);
    timer.stop("subdomain.vector_operations");
}

template<typename DType>
void Subdomain<DType>::initialize_arrays(occa::memory &u_k, occa::memory &r_k, occa::memory &f)
{
    int num_values = subdomain_operator.num_points + superdomain_operator.num_extended_dofs;
    initialize_arrays_kernel(u_k, r_k, f, num_values);
}

template<typename DType>
void Subdomain<DType>::assembled_inner_product(DType &uv, occa::memory &u, occa::memory &v)
{
    occa::memory u_sub_l = u.slice(0, subdomain_operator.num_points);
    occa::memory u_sup = u.slice(subdomain_operator.num_points, superdomain_operator.num_extended_dofs);
    occa::memory u_work_sub = work_dev[0].slice(0, subdomain_operator.num_extended_dofs);
    occa::memory u_work_sup = work_dev[0].slice(subdomain_operator.num_extended_dofs, superdomain_operator.num_extended_dofs);

    subdomain_operator.Qt.multiply_weight(u_work_sub, u_sub_l, norm_weight);
    u_sup.copyTo(u_work_sup, superdomain_operator.num_extended_dofs * sizeof(DType));

    occa::memory v_sub_l = v.slice(0, subdomain_operator.num_points);
    occa::memory v_sup = v.slice(subdomain_operator.num_points, superdomain_operator.num_extended_dofs);
    occa::memory v_work_sub = work_dev[1].slice(0, subdomain_operator.num_extended_dofs);
    occa::memory v_work_sup = work_dev[1].slice(subdomain_operator.num_extended_dofs, superdomain_operator.num_extended_dofs);

    subdomain_operator.Qt.multiply_weight(v_work_sub, v_sub_l, norm_weight);
    v_sup.copyTo(v_work_sup, superdomain_operator.num_extended_dofs * sizeof(DType));

    uv = 0.0;

    int num_values = subdomain_operator.num_extended_dofs + superdomain_operator.num_extended_dofs;
    int num_blocks = (num_values + BLOCK_SIZE - 1) / BLOCK_SIZE;

    occa::memory &temp = p_k;
    weighted_inner_product_kernel(temp, work_dev[0], work_dev[1], norm_weight, num_values, num_blocks);

    temp.copyTo(work_hst[0].data(), num_blocks * sizeof(DType));

    for (int b = 0; b < num_blocks; b++) uv += work_hst[0][b];
}

template<typename DType>
void Subdomain<DType>::generalized_minimum_residual(occa::memory &u_l, occa::memory &f_l, bool print_history, bool use_relative)
{
    // Collect tree data
    tree_operator(f, f_l);

    // Initialize arrays
    timer.start("subdomain.vector_operations");
    initialize_arrays(u_k, r_k, f);
    timer.stop("subdomain.vector_operations");

    // Compute initial residual
    DType r_norm;
    DType r_0_norm;

    timer.start("subdomain.residual_norm");
    residual_norm(r_0_norm, r_k);
    timer.stop("subdomain.residual_norm");

    if (print_history) pstdout("- Iter %3d: | residual_norm = %24.16g | relative_residual_norm = %24.16g | \n", 0, r_0_norm, 1.0);

    // Iterative solver
    bool converged = false;
    int iter = 0;
    int outer = 0;
    int j;

    DType alpha_j;
    DType beta_j;
    DType gamma_j;
    DType gamma_k;

    while (iter < max_iterations)
    {
        if (iter > 0)
        {
            timer.start("subdomain.operator_application");
            stiffness_matrix(r_k, u_k);
            timer.stop("subdomain.operator_application");

            timer.start("subdomain.vector_operations");
            math.vector_vector_addition(r_k, 1.0, f, - 1.0, r_k, num_values);
            timer.stop("subdomain.vector_operations");

            timer.start("subdomain.residual_norm");
            residual_norm(r_norm, r_k);
            timer.stop("subdomain.residual_norm");

            gamma[0] = r_norm;
        }
        else
        {
            gamma[0] = r_0_norm;
        }

        timer.start("subdomain.vector_operations");
        math.vector_scaling(V[0], 1.0 / gamma[0], r_k, num_values);
        timer.stop("subdomain.vector_operations");

        for (j = 0; j < num_vectors; j++)
        {
            iter++;


            if (use_preconditioner)
            {
                low_order_preconditioner(Z[j], V[j]);
            }
            else
            {
                timer.start("subdomain.preconditioner.identity");
                direct_stiffness_summation(Z[j], V[j]);
                timer.stop("subdomain.preconditioner.identity");
            }

            timer.start("subdomain.operator_application");
            stiffness_matrix(q_k, Z[j]);
            timer.stop("subdomain.operator_application");

            // 2-pass Gram-Schmidt (1st pass)
            for (int i = 0; i < j + 1; i++)
            {
                timer.start("subdomain.inner_products");
                assembled_inner_product(H[i][j], q_k, V[i]);
                timer.stop("subdomain.inner_products");
            }

            for (int i = 0; i < j + 1; i++)
            {
                timer.start("subdomain.vector_operations");
                math.vector_vector_addition(q_k, 1.0, q_k, - H[i][j], V[i], num_values);
                timer.stop("subdomain.vector_operations");
            }

            // Apply Given's rotation to new column
            for (int i = 0; i < j; i++)
            {
                DType h_ij = H[i][j];
                H[i][j] = c_gmres[i] * h_ij + s_gmres[i] * H[i + 1][j];
                H[i + 1][j] = - s_gmres[i] * h_ij + c_gmres[i] * H[i + 1][j];
            }

            timer.start("subdomain.residual_norm");
            residual_norm(alpha_j, q_k);
            timer.stop("subdomain.residual_norm");

            if (std::abs(alpha_j) == 0.0)
            {
                converged = true;
                break;
            }

            beta_j = std::sqrt(H[j][j] * H[j][j] + alpha_j * alpha_j);
            gamma_j = 1.0 / beta_j;
            c_gmres[j] = H[j][j] * gamma_j;
            s_gmres[j] = alpha_j * gamma_j;
            H[j][j] = beta_j;
            gamma[j + 1] = - s_gmres[j] * gamma[j];
            gamma[j] = c_gmres[j] * gamma[j];
    
            r_norm = std::abs(gamma[j + 1]);
            if (print_history) pstdout("- Iter %3d: | residual_norm = %24.16g | relative_residual_norm = %24.16g | \n", iter, r_norm, r_norm / r_0_norm);

            if (use_relative)
            {
                if (r_norm / r_0_norm < tolerance)
                {
                    converged = true;
                    break;
                }
            }
            else
            {
                if (r_norm < tolerance)
                {
                    converged = true;
                    break;
                }
            }

            if (iter >= max_iterations)
            {
                converged = true;
                break;
            }

            timer.start("subdomain.vector_operations");
            math.vector_scaling(V[j + 1], 1.0 / alpha_j, q_k, num_values);
            timer.stop("subdomain.vector_operations");
        }

        if (j == num_vectors) j--;

        for (int k = j; k >= 0; k--)
        {
            gamma_k = gamma[k];

            for (int i = j; i > k; i--)
                gamma_k -= H[k][i] * c_gmres[i];

            c_gmres[k] = gamma_k / H[k][k];
        }

        // Sum Arnoldi vectors
        for (int i = 0; i < j + 1; i++)
        {
            timer.start("subdomain.vector_operations");
            math.vector_vector_addition(u_k, 1.0, u_k, c_gmres[i], Z[i], num_values);
            timer.stop("subdomain.vector_operations");
        }

        if (converged) break;
        outer++;
    }

    timer.start("subdomain.vector_operations");
    copy_to_domain_data_kernel(u_l, u_k, levels[0].num_points);
    timer.stop("subdomain.vector_operations");

    num_iterations += iter;
}

template<typename DType>
void Subdomain<DType>::residual_norm(DType &r_norm, occa::memory &r)
{
    occa::memory r_sub_l = r.slice(0, subdomain_operator.num_points);
    occa::memory r_sup = r.slice(subdomain_operator.num_points, superdomain_operator.num_extended_dofs);

    occa::memory work_sub = work_dev[1].slice(0, subdomain_operator.num_extended_dofs);
    occa::memory work_sup = work_dev[1].slice(subdomain_operator.num_extended_dofs, superdomain_operator.num_extended_dofs);

    subdomain_operator.Qt.multiply_weight(work_sub, r_sub_l, norm_weight);
    r_sup.copyTo(work_sup, superdomain_operator.num_extended_dofs * sizeof(DType));

    r_norm = 0.0;

    int num_values = subdomain_operator.num_extended_dofs + superdomain_operator.num_extended_dofs;
    int num_blocks = (num_values + BLOCK_SIZE - 1) / BLOCK_SIZE;

    weighted_inner_product_kernel(work_dev[0], work_dev[1], work_dev[1], norm_weight, num_values, num_blocks);

    work_dev[0].copyTo(work_hst[0].data(), num_blocks * sizeof(DType));

    for (int b = 0; b < num_blocks; b++) r_norm += work_hst[0][b];

    r_norm = std::sqrt(r_norm);
}

template<typename DType>
void Subdomain<DType>::projection_inner_products(DType &gamma_k, DType &theta_k, occa::memory &z_k, occa::memory &r_k, occa::memory &p_k, occa::memory &q_k)
{
    int num_values = subdomain_operator.num_points + superdomain_operator.num_extended_dofs;
    int num_blocks = (num_values + BLOCK_SIZE - 1) / BLOCK_SIZE;

    gamma_k = 0.0;
    theta_k = 0.0;

    projection_inner_products_kernel(work_dev[0], z_k, r_k, p_k, q_k, inner_weight, num_values, num_blocks);

    work_dev[0].copyTo(work_hst[0].data(), (2 * num_blocks) * sizeof(DType));

    for (int b = 0; b < num_blocks; b++)
    {
        gamma_k += work_hst[0][b];
        theta_k += work_hst[0][b + num_blocks];
    }
}

template<typename DType>
void Subdomain<DType>::solution_and_residual_update(occa::memory &u_k, occa::memory &r_kp1, occa::memory &r_k, occa::memory &p_k, occa::memory &q_k, DType alpha_k)
{
    int num_values = subdomain_operator.num_points + superdomain_operator.num_extended_dofs;
    solution_and_residual_update_kernel(u_k, r_kp1, r_k, p_k, q_k, alpha_k, num_values);
}

template<typename DType>
void Subdomain<DType>::search_update_inner_product(DType &theta_k, occa::memory &r_k, occa::memory &r_kp1, occa::memory &z_k)
{
    int num_values = subdomain_operator.num_points + superdomain_operator.num_extended_dofs;
    int num_blocks = (num_values + BLOCK_SIZE - 1) / BLOCK_SIZE;

    theta_k = 0.0;

    search_update_inner_product_kernel(work_dev[0], r_k, r_kp1, z_k, inner_weight, num_values, num_blocks);

    work_dev[0].copyTo(work_hst[0].data(), num_blocks * sizeof(DType));

    for (int b = 0; b < num_blocks; b++) theta_k += work_hst[0][b];
}

template<typename DType>
void Subdomain<DType>::residual_and_search_update(occa::memory &p_k, occa::memory &r_k, occa::memory &z_k, occa::memory &r_kp1, DType beta_k)
{
    int num_values = subdomain_operator.num_points + superdomain_operator.num_extended_dofs;
    residual_and_search_update_kernel(p_k, r_k, z_k, r_kp1, beta_k, num_values);
}

template<typename DType>
void Subdomain<DType>::tree_operator(occa::memory &Tu, occa::memory &u)
{
    // Fill up tree
    timer.start("subdomain.tree_construction.gpu_to_gpu");
    copy_from_domain_data_kernel(work_dev[0], u, levels[0].num_points);
    timer.stop("subdomain.tree_construction.gpu_to_gpu");

    timer.start("subdomain.tree_construction.subdomain");

    for (int l = 0; l < num_levels - 1; l++)
    {
        int N_f = levels[l].poly_degree;
        int N_c = levels[l + 1].poly_degree;
        int n_f = N_f + 1;
        int n_c = N_c + 1;
        int num_points;

        std::pair<int, int> idx(N_c, N_f);
        occa::memory &J_cf_l = J_cf[idx].second;

        occa::memory u_f = work_dev[0].slice(levels[l + 0].offset, levels[l + 0].num_points);
        occa::memory u_c = work_dev[0].slice(levels[l + 1].offset, levels[l + 1].num_points);

        if (dim == 2)
        {
            num_points = levels[l].num_elements * (n_f * n_c);
            restriction_1_kernel(work_dev[1], J_cf_l, u_f, num_points, n_f, n_c);

            num_points = levels[l].num_elements * (n_c * n_c);
            restriction_2_kernel(u_c, J_cf_l, work_dev[1], num_points, n_f, n_c);
        }
        else
        {
            num_points = levels[l].num_elements * (n_f * n_f * n_c);
            restriction_1_kernel(work_dev[1], J_cf_l, u_f, num_points, n_f, n_c);

            num_points = levels[l].num_elements * (n_f * n_c * n_c);
            restriction_2_kernel(work_dev[2], J_cf_l, work_dev[1], num_points, n_f, n_c);

            num_points = levels[l].num_elements * (n_c * n_c * n_c);
            restriction_3_kernel(u_c, J_cf_l, work_dev[2], num_points, n_f, n_c);
        }
    }

    timer.stop("subdomain.tree_construction.subdomain");

    timer.start("subdomain.tree_construction.gpu_to_cpu");
    int total_level_points = levels[num_levels - 1].offset + levels[num_levels - 1].num_points;
    work_dev[0].copyTo(work_hst[0].data(), total_level_points * sizeof(DType));
    timer.stop("subdomain.tree_construction.gpu_to_cpu");

    // Get coarse grid
    timer.start("subdomain.tree_exchange.superdomain");
    memcpy(work_hst[1].data() + proc_offset[proc_id], work_hst[0].data() + levels[num_levels - 1].offset, levels[num_levels - 1].num_points * sizeof(DType));
    MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, work_hst[1].data(), proc_count.data(), proc_offset.data(), (typeid(DType) == typeid(double)) ? MPI_DOUBLE : MPI_FLOAT, MPI_COMM_WORLD);
    timer.stop("subdomain.tree_exchange.superdomain");

    // Subdomain data
    timer.start("subdomain.tree_exchange.subdomain");
    gslib_gs(work_hst[0].data(), gs_type, gs_add, 0, gs_handle, NULL);
    timer.stop("subdomain.tree_exchange.subdomain");

    timer.start("subdomain.tree_exchange.cpu_to_gpu");
    Tu.copyFrom(work_hst[0].data() + total_level_points, subdomain_operator.num_points * sizeof(DType));
    timer.stop("subdomain.tree_exchange.cpu_to_gpu");

    // Superdomain data
    timer.start("subdomain.tree_exchange.cpu_to_gpu");
    work_dev[0].copyFrom(work_hst[1].data(), Qt_coarse.num_cols * sizeof(DType));
    timer.stop("subdomain.tree_exchange.cpu_to_gpu");

    timer.start("subdomain.tree_construction.assemble_coarse");
    Qt_coarse.multiply(work_dev[1], work_dev[0]);
    timer.stop("subdomain.tree_construction.assemble_coarse");

    timer.start("subdomain.tree_construction.superdomain");
    occa::memory Tu_sup = Tu.slice(subdomain_operator.num_points, superdomain_operator.num_extended_dofs);
    superdomain_operator.Pt.multiply(Tu_sup, work_dev[1]);
    timer.stop("subdomain.tree_construction.superdomain");
}

// Visit output
template<typename DType>
void Subdomain<DType>::output(std::string output_name, int num_fields, ...)
{
    // Silo database
    DBSetDeprecateWarnings(0);
    DBfile *silo_file = NULL;
    char silo_name[80];

    sprintf(silo_name, "%s.%d.silo", output_name.c_str(), proc_id);
    silo_file = DBCreate(silo_name, DB_CLOBBER, DB_LOCAL, "Field data", DB_PDB);

    if (silo_file == NULL)
    {
        printf("ERROR: Couldn't create Silo file for \"p = %d\"\n", proc_id);

        MPI_Finalize();
        exit(EXIT_FAILURE);
    }

    // Mesh
    int num_vertices = (dim == 2) ? 4 : 8;
    int num_points = 0; for (auto &elem : elements) num_points += elem.num_points;
    int num_elements = (int)(elements.size());
    int num_low_order_elems = 0; for (auto &elem : elements) num_low_order_elems += std::pow(elem.poly_degree, dim);
    int num_low_order_points = num_low_order_elems * num_vertices;

    std::vector<int> element_offset(num_elements);
    for (int e = 1; e < num_elements; e++) element_offset[e] = element_offset[e - 1] + elements[e - 1].num_points;

    std::vector<int> low_order_elements(num_low_order_points);
    int offset = 0;

    if (dim == 2)
    {
        for (int e = 0; e < num_elements; e++)
        {
            auto &elem = elements[e];
            int n_x = elem.n_x;

            for (int s_y = 0; s_y < elem.poly_degree; s_y++)
            {
                for (int s_x = 0; s_x < elem.poly_degree; s_x++)
                {
                    low_order_elements[offset++] = element_offset[e] + (s_x + 0) + (s_y + 0) * n_x;
                    low_order_elements[offset++] = element_offset[e] + (s_x + 1) + (s_y + 0) * n_x;
                    low_order_elements[offset++] = element_offset[e] + (s_x + 1) + (s_y + 1) * n_x;
                    low_order_elements[offset++] = element_offset[e] + (s_x + 0) + (s_y + 1) * n_x;
                }
            }
        }
    }
    else
    {
        for (int e = 0; e < num_elements; e++)
        {
            auto &elem = elements[e];
            int n_x = elem.n_x;
            int n_xy = n_x * n_x;

            for (int s_z = 0; s_z < elem.poly_degree; s_z++)
            {
                for (int s_y = 0; s_y < elem.poly_degree; s_y++)
                {
                    for (int s_x = 0; s_x < elem.poly_degree; s_x++)
                    {
                        low_order_elements[offset++] = element_offset[e] + (s_x + 0) + (s_y + 0) * n_x + (s_z + 0) * n_xy;
                        low_order_elements[offset++] = element_offset[e] + (s_x + 1) + (s_y + 0) * n_x + (s_z + 0) * n_xy;
                        low_order_elements[offset++] = element_offset[e] + (s_x + 1) + (s_y + 1) * n_x + (s_z + 0) * n_xy;
                        low_order_elements[offset++] = element_offset[e] + (s_x + 0) + (s_y + 1) * n_x + (s_z + 0) * n_xy;
                        low_order_elements[offset++] = element_offset[e] + (s_x + 0) + (s_y + 0) * n_x + (s_z + 1) * n_xy;
                        low_order_elements[offset++] = element_offset[e] + (s_x + 1) + (s_y + 0) * n_x + (s_z + 1) * n_xy;
                        low_order_elements[offset++] = element_offset[e] + (s_x + 1) + (s_y + 1) * n_x + (s_z + 1) * n_xy;
                        low_order_elements[offset++] = element_offset[e] + (s_x + 0) + (s_y + 1) * n_x + (s_z + 1) * n_xy;
                    }
                }
            }
        }
    }

    std::vector<DType> x, y, z;

    if (dim >= 1)
    {
        x.resize(num_points);

        for (int e = 0; e < num_elements; e++)
        {
            auto &elem = elements[e];
            memcpy(x.data() + element_offset[e], elem.x.data(), elem.num_points * sizeof(DType));
        }
    }

    if (dim >= 2)
    {
        y.resize(num_points);

        for (int e = 0; e < num_elements; e++)
        {
            auto &elem = elements[e];
            memcpy(y.data() + element_offset[e], elem.y.data(), elem.num_points * sizeof(DType));
        }
    }

    if (dim >= 3)
    {
        z.resize(num_points);

        for (int e = 0; e < num_elements; e++)
        {
            auto &elem = elements[e];
            memcpy(z.data() + element_offset[e], elem.z.data(), elem.num_points * sizeof(DType));
        }
    }

    DType *coordinates[] = { x.data(), y.data(), z.data() };
    DBPutZonelist(silo_file, "elements", num_low_order_elems, dim, low_order_elements.data(), num_low_order_points, 0, &num_vertices, &num_low_order_elems, 1);
    DBPutUcdmesh(silo_file, "mesh", dim, NULL, coordinates, num_points, num_low_order_elems, "elements", NULL, (typeid(DType) == typeid(double)) ? DB_DOUBLE : DB_FLOAT, NULL);
    
    // Fields
    char *field_name;
    occa::memory field_data;

    va_list args;
    va_start(args, num_fields);

    for (int field = 0; field < num_fields; field++)
    {
        field_name = va_arg(args, char*);
        field_data = va_arg(args, occa::memory);
        field_data.copyTo(work_hst[0].data(), num_points * sizeof(DType));

        for (auto &elem : elements)
            for (int vid = 0; vid < elem.num_points; vid++)
                work_hst[1][elem.offset + vid] = work_hst[0][elem.loc_num[vid]];

        DBPutUcdvar1(silo_file, field_name, "mesh", work_hst[1].data(), num_points, NULL, 0, (typeid(DType) == typeid(double)) ? DB_DOUBLE : DB_FLOAT, DB_NODECENT, NULL);
    }

    va_end(args);

    // Free data
    DBClose(silo_file);
}
