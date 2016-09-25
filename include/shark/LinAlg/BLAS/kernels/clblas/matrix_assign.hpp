/*!
 * \brief       Kernels for matrix-expression assignments
 * 
 * \author      O. Krause
 * \date        2013
 *
 *
 * \par Copyright 1995-2015 Shark Development Team
 * 
 * <BR><HR>
 * This file is part of Shark.
 * <http://image.diku.dk/shark/>
 * 
 * Shark is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published 
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Shark is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with Shark.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#ifndef SHARK_LINALG_BLAS_KERNELS_MATRIX_ASSIGN_HPP
#define SHARK_LINALG_BLAS_KERNELS_MATRIX_ASSIGN_HPP

#include <boost/compute/detail/meta_kernel.hpp>

namespace shark {namespace blas {namespace bindings{
	
//////////////////////////////////////////////////////
////Scalar Assignment to Matrix
/////////////////////////////////////////////////////

// Explicitly iterating row major
template<class F, class M>
void matrix_assign(
	matrix_expression<M, gpu_tag> &m, 
	typename M::value_type t, 
	row_major, dense_tag
){
	typedef M::value_type value_type;
	boost::compute::detail::meta_kernel k("blas_matrix_assign_constant");
	std::size_t t_index = k.add_arg<value_type>('t');
	
	//create source
	auto exprRow=k.expr<cl_uint>("get_global_id(0)");
	auto exprCol=k.expr<cl_uint>("get_global_id(1)");
	k<< m()(exprRow,exprCol) <<'=' << F(m()(exprRow,exprCol), k.var<value_type>('t'))<<";"
	
	boost::compute::kernel kernel = k.compile(queue.get_context());
	//enqueue kernel
	k.set_arg(t_index, t);
	std::size_t global_work_size[2] = {rows, cols};
	m().queue().enqueue_nd_rnge_kernel(kernel, 2,nullptr, global_work_size, nullptr);
}


///////////////////////////////////////////////////////////////////////////////////////////
//////Matrix Assignment With Functor implementing +=,-=...
///////////////////////////////////////////////////////////////////////////////////////////

//dense-dense case row-major, row-major
template<class F, class M, class E>
void matrix_assign_functor(
	matrix_expression<M, gpu_tag> &m, 
	matrix_expression<E, gpu_tag> const& e,
	row_major, row_major,dense_tag, dense_tag
) {
	F f;
	//create source
	boost::compute::detail::meta_kernel k("blas_matrix_assign_row_row");
	auto exprRow=k.expr<cl_uint>("get_global_id(0)");
	auto exprCol=k.expr<cl_uint>("get_global_id(1)");
	k<< m()(exprRow,exprCol) <<'=' << f(m()(exprRow,exprCol),e()(exprRow,exprCol))<<";"
	//enqueue kernel
	boost::compute::kernel kernel = k.compile(queue.get_context());
	std::size_t global_work_size[2] = {rows, cols};
	m().queue().enqueue_nd_rnge_kernel(kernel, 2,nullptr, global_work_size, nullptr);
}

//dense-dense case row-major, column-major
template<class F,class M, class E>
void matrix_assign_functor(
	matrix_expression<M, gpu_tag> &m, 
	matrix_expression<E, gpu_tag> const& e,
	row_major, column_major,dense_tag, dense_tag
) {
	//Kernel is based on boost/compute/examples/matrix_transpose.cpp
	typedef M::value_type value_type;
	F f;
	std::size_t TILE_DIM = 32;
	//There are usually not enough parallel worker threads in a local group
	//to fill a tile. Therefore every parallel threads reads several elements.
	//BLOCK_COLS are the number of parallel threads reading a column
	//and must be a divisor of TILE_DIM
	std::size_t BLOCK_COLS = 8; 
	//this kernel only works for matrix sizes that are divisable by TILE_DIM
	SIZE_CHECK(m().size1() % TILE_DIM == 0);
	SIZE_CHECK(m().size2() % TILE_DIM == 0);
	
	
	//create source
	boost::compute::detail::meta_kernel k("blas_matrix_assign_row_col");
	//create local memory. we first copy a tile in local
	// memory which gets the orientation right. Then we copy the tile
	//to the target
	// TILE_DIM+1 is here to avoid bank conflicts in local memory
	k << "__local" <<k.decl<value_type>("tile")<< "[TILE_DIM][TILE_DIM+1]";
	k << "uint base_row = get_group_id(0) * TILE_DIM;";
	k << "uint base_col = get_group_id(1) * TILE_DIM;";
	//copy indices, into local memory, note the change of direction
	k << "for(uint i = 0 ; i < TILE_DIM; i += get_local_size(1)){";
	auto row_exp = k.expr<cl_uint>("base_row+get_local_id(1)+i");
	auto col_exp = k.expr<cl_uint>("base_row+get_local_id(1)+i");
	k << "    tile[get_local_id(1)+i][get_local_id(0)] =" << e()(row_exp, col_exp)<<';';
	k << '}';
	k << "barrier(CLK_LOCAL_MEM_FENCE);";//wait until all threads are done with copying
	k << "for(uint i = 0 ; i < TILE_DIM ; i += get_local_size(1)){"; // write output from local memory
	auto target = m()(k.expr<cl_uint>("base_row + get_local_id(0)"), k.expr<cl_uint>("base_col + get_local_id(1)+i"))
	k << target << " = "f(target, k.expr<cl_uint>("tile[get_local_id(0)][get_local_id(1)+i];");
	k << '}';
	
	//compile kernel
	char const* options ="-DTILE_DIM = 32";
	boost::compute::kernel kernel = k.compile(queue.get_context(), options);
	
	//enqueue kernel
	std::size_t global_work_size[2] = {rows, cols * BLOCK_COLS / TILE_DIM};
	std::size_t local_work_size[2] = {TILE_DIM, BLOCK_COLS};
	m().queue().enqueue_nd_rnge_kernel(kernel, 2,nullptr, global_work_size, local_work_size);
}

/////////////////////////////////////////////////////////////////
//////Matrix Assignment implementing op=
////////////////////////////////////////////////////////////////

//implement by using the assigner function below and call the functions above

namespace detail {
struct assigner{
	template<class Arg1, class Arg2>
	Arg2 operator()(Arg1 const&, Arg2 const& y) const{
		return y;
	}
};
}

template<class M, class E>
void matrix_assign(
	matrix_expression<M, gpu_tag> &m, 
	matrix_expression<E, gpu_tag> const& e,
	row_major o, row_major,dense_tag t, dense_tag
) {
	matrix_assign_functor<detail::assigner>(m,e,o,o,t,t);
}

//dense-dense case
template<class M, class E>
void matrix_assign(
	matrix_expression<M, gpu_tag> &m, 
	matrix_expression<E, gpu_tag> const& e,
	row_major o1, column_major o2,dense_tag t, dense_tag
) {
	matrix_assign_functor<detail::assigner>(m,e,o1,o2,t,t);
}


}}}

#endif
