#ifndef AMGCL_AMGCL_HPP
#define AMGCL_AMGCL_HPP

/*
The MIT License

Copyright (c) 2012 Denis Demidov <ddemidov@ksu.ru>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   amgcl.hpp
 * \author Denis Demidov <ddemidov@ksu.ru>
 * \brief  Generic algebraic multigrid framework.
 */

/**
\mainpage amgcl Generic algebraic multigrid framework.

This is a simple and generic AMG hierarchy builder (and a work in progress).
May be used as a standalone solver or as a preconditioner. CG and BiCGStab
iterative solvers are provided. Solvers from <a
href="http://viennacl.sourceforge.net">ViennaCL</a> are supported as well.

<a href="https://github.com/ddemidov/vexcl">VexCL</a>, <a
href="http://viennacl.sourceforge.net">ViennaCL</a>, or <a
href="http://eigen.tuxfamily.org">Eigen</a> matrix/vector
containers may be used with built-in and ViennaCL's solvers. See
<a href="https://github.com/ddemidov/amgcl/blob/master/examples/vexcl.cpp">examples/vexcl.cpp</a>,
<a href="https://github.com/ddemidov/amgcl/blob/master/examples/viennacl.cpp">examples/viennacl.cpp</a> and
<a href="https://github.com/ddemidov/amgcl/blob/master/examples/eigen.cpp">examples/eigen.cpp</a> for respective examples.

\section setup AMG hierarchy building

Constructor of amgcl::solver<> object builds the multigrid hierarchy based on
algebraic information contained in the system matrix:

\code
// amgcl::sparse::matrix<double, int> A;
// or
// amgcl::sparse::matrix_map<double, int> A;
amgcl::solver<
    double,                 // Scalar type
    int,                    // Index type of the matrix
    amgcl::interp::classic, // Interpolation kind
    amgcl::level::cpu       // Where to store the hierarchy
> amg(A);
\endcode

See documentation for \ref interpolation "Interpolation" module to see the list
of supported interpolation schemes. The aggregation schemes use less memory and
are set up faster than classic interpolation, but their convergence rate is
slower. They are well suited for VexCL or ViennaCL containers, where solution
phase is accelerated by the OpenCL technology and, therefore, the cost of the
setup phase is much more important.

\code
amgcl::solver<
    double, int,
    amgcl::interp::smoothed_aggregation<amgcl::aggr::plain>,
    amgcl::level::vexcl
> amg(A);
\endcode

\section solution Solution

Once the hierarchy is constructed, it may be repeatedly used to solve the
linear system for different right-hand sides:

\code
// std::vector<double> rhs, x;

auto conv = amg.solve(rhs, x);

std::cout << "Iterations: " << std::get<0>(conv) << std::endl
          << "Error:      " << std::get<1>(conv) << std::endl;
\endcode

Using the AMG as a preconditioner with a Krylov subspace method like conjugate
gradients works even better:
\code
// Eigen::VectorXd rhs, x;

auto conv = amgcl::solve(A, rhs, amg, x, amgcl::cg_tag());
\endcode

Types of right-hand side and solution vectors should be compatible with the
level type used for construction of the AMG hierarchy. For example,
if amgcl::level::vexcl is used as a storage backend, then vex::SpMat<> and
vex::vector<> types have to be used when solving:

\code
// vex::SpMat<double,int> Agpu;
// vex::vector<double> rhs, x;

auto conv = amgcl::solve(Agpu, rhs, amg, x, amgcl::cg_tag());
\endcode

\section install Installation

The library is header-only, so there is nothing to compile or link to. You just
need to copy amgcl folder somewhere and tell your compiler to scan it for
include files.

\section references References
 -# \anchor Trottenberg_2001 <em>U. Trottenberg, C. Oosterlee, A. Shuller,</em>
    Multigrid, Academic Press, London, 2001.
 -# \anchor Stuben_1999 <em>K. Stuben,</em> Algebraic multigrid (AMG): an
    introduction with applications, Journal of Computational and Applied
     Mathematics,  2001, Vol. 128, Pp. 281-309.
 -# \anchor Vanek_1996 <em>P. Vanek, J. Mandel, M. Brezina,</em> Algebraic multigrid
    by smoothed aggregation for second and fourth order elliptic problems,
    Computing 56, 1996, Pp. 179-196.
 -# \anchor Notay_2008 <em>Y. Notay, P. Vassilevski,</em> Recursive
    Krylov-based multigrid cycles, Numer. Linear Algebra Appl. 2008; 15:473-487.
 -# \anchor Templates_1994 <em>R. Barrett, M. Berry,
    T. F. Chan et al.</em> Templates for the Solution of Linear Systems:
    Building Blocks for Iterative Methods, 2nd Edition, SIAM, Philadelphia, PA,
    1994.
*/

#include <iostream>
#include <iomanip>
#include <utility>
#include <list>

#include <boost/static_assert.hpp>
#include <boost/typeof/typeof.hpp>
#include <boost/type_traits/is_signed.hpp>

#include <amgcl/spmat.hpp>
#include <amgcl/profiler.hpp>

/// Primary namespace for the library.
namespace amgcl {

/// Interpolation-related types and functions.
namespace interp {

/// Galerkin operator.
struct galerkin_operator {
    template <class spmat, class Params>
    static spmat apply(const spmat &R, const spmat &A, const spmat &P,
            const Params &prm)
    {
        return sparse::prod(sparse::prod(R, A), P);
    }
};

/// Returns coarse level construction scheme for a given interpolation scheme.
/**
 * By default, Galerkin operator is used to construct coarse level from system
 * matrix, restriction and prolongation operators:
 * \f[A^H = R A^h P.\f] Usually, \f$R = P^T.\f$
 *
 * \param Interpolation interpolation scheme.
 */
template <class Interpolation>
struct coarse_operator {
    typedef galerkin_operator type;
};

} // namespace interp

/// Algebraic multigrid method.
/**
 * \param value_t  Type for matrix entries (double/float).
 * \param index_t  Type for matrix indices. Should be signed integral type.
 * \param interp_t \ref interpolation "Interpolation scheme".
 * \param level_t  Hierarchy level \ref levels "storage backend".
 */
template <
    typename value_t, typename index_t, typename interp_t, typename level_t
    >
class solver {
    private:
        typedef sparse::matrix<value_t, index_t> matrix;
        typedef typename level_t::template instance<value_t, index_t> level_type;

    public:
        /// Parameters for AMG components.
        struct params {
            /// When level is coarse enough to be solved directly?
            /**
             * If number of variables at a next level in hierarchy becomes
             * lower than this threshold, then the hierarchy construction is
             * stopped and the linear system is solved explicitly at this
             * level.
             */
            unsigned coarse_enough;

            typename interp_t::params interp; ///< Interpolation parameters.
            typename level_t::params  level;  ///< Level/Solution parameters.

            params() : coarse_enough(300) { }
        };

        /// Constructs the AMG hierarchy from the system matrix.
        /** 
         * The input matrix is copied here and may be freed afterwards.
         *
         * \param A   The system matrix. Should be convertible to
         *            amgcl::sparse::matrix<>.
         * \param prm Parameters controlling the setup and solution phases.
         *
         * \sa amgcl::sparse::map()
         */
        template <typename spmat>
        solver(const spmat &A, const params &prm = params()) : prm(prm)
        {
            BOOST_STATIC_ASSERT_MSG(boost::is_signed<index_t>::value,
                    "Matrix index type should be signed");

            matrix copy(A);
            build_level(copy, prm);
        }

        /// The AMG hierarchy is used as a standalone solver.
        /** 
         * The vector types should be compatible with level_t:
         *
         * -# Any type with operator[] should work on a CPU.
         * -# vex::vector<value_t> should be used with level::vexcl.
         * -# viennacl::vector<value_t> should be used with level::ViennaCL.
         *
         * \param rhs Right-hand side.
         * \param x   Solution. Contains an initial approximation on input, and
         *            the approximated solution on output.
         */
        template <class vector1, class vector2>
        std::pair< int, value_t > solve(const vector1 &rhs, vector2 &x) const {
            int     iter = 0;
            value_t res  = 2 * prm.level.tol;

            for(; res > prm.level.tol && iter < prm.level.maxiter; ++iter) {
                apply(rhs, x);
                res = hier.front().resid(rhs, x);
            }

            return std::make_pair(iter, res);
        }

        /// Performs single multigrid cycle.
        /**
         * Is intended to be used as a preconditioner with iterative methods.
         *
         * The vector types should be compatible with level_t:
         *
         * -# Any type with operator[] should work on a CPU.
         * -# vex::vector<value_t> should be used with level::vexcl.
         * -# viennacl::vector<value_t> should be used with level::ViennaCL.
         *
         * \param rhs Right-hand side.
         * \param x   Solution. Contains an initial approximation on input, and
         *            the approximated solution on output.
         */
        template <class vector1, class vector2>
        void apply(const vector1 &rhs, vector2 &x) const {
            level_type::cycle(hier.begin(), hier.end(), prm.level, rhs, x);
        }

        /// Output some general information about the AMG hierarchy.
        std::ostream& print(std::ostream &os) const {
            BOOST_AUTO(ff, os.flags());
            BOOST_AUTO(pp, os.precision());

            index_t sum_dof = 0;
            index_t sum_nnz = 0;
            for(BOOST_AUTO(lvl, hier.begin()); lvl != hier.end(); ++lvl) {
                sum_dof += lvl->size();
                sum_nnz += lvl->nonzeros();
            }

            os << "Number of levels:    "   << hier.size()
               << "\nOperator complexity: " << std::fixed << std::setprecision(2)
                                            << 1.0 * sum_nnz / hier.front().nonzeros()
               << "\nGrid complexity:     " << std::fixed << std::setprecision(2)
                                            << 1.0 * sum_dof / hier.front().size()
               << "\n\nlevel     unknowns       nonzeros\n"
               << "---------------------------------\n";

            index_t depth = 0;
            for(BOOST_AUTO(lvl, hier.begin()); lvl != hier.end(); ++lvl, ++depth)
                os << std::setw(5)  << depth
                   << std::setw(13) << lvl->size()
                   << std::setw(15) << lvl->nonzeros() << " ("
                   << std::setw(5) << std::fixed << std::setprecision(2)
                   << 100.0 * lvl->nonzeros() / sum_nnz
                   << "%)" << std::endl;

            os.flags(ff);
            os.precision(pp);
            return os;
        }
    private:
        void build_level(matrix &A, const params &prm, unsigned nlevel = 0)
        {
            if (A.rows <= prm.coarse_enough) {
                matrix Ai = sparse::inverse(A);
#if defined(__GXX_EXPERIMENTAL_CXX0X__) || __cpluplus >= 201103L
                hier.emplace_back( A, Ai, prm.level, nlevel );
#else
                hier.push_back( level_type(A, Ai, prm.level, nlevel) );
#endif
            } else {
                TIC("interp");
                matrix P = interp_t::interp(A, prm.interp);
                TOC("interp");

                TIC("transp");
                matrix R = sparse::transpose(P);
                TOC("transp");

                TIC("coarse operator");
                matrix a = interp::coarse_operator<interp_t>::type::apply(
                        R, A, P, prm.interp);
                TOC("coarse operator");

#if defined(__GXX_EXPERIMENTAL_CXX0X__) || __cpluplus >= 201103L
                hier.emplace_back( A, P, R, prm.level, nlevel );
#else
                hier.push_back( level_type(A, P, R, prm.level, nlevel) );
#endif

                build_level(a, prm, nlevel + 1);
            }
        }

        params prm;
        std::list< level_type > hier;
};

} // namespace amgcl

/// Output some general information about the AMG hierarchy.
template <
    typename value_t,
    typename index_t,
    typename interp_t,
    typename level_t
    >
std::ostream& operator<<(std::ostream &os, const amgcl::solver<value_t, index_t, interp_t, level_t> &amg) {
    return amg.print(os);
}

#endif
