#ifndef AMGCL_AMGCL_HPP
#define AMGCL_AMGCL_HPP

/*
The MIT License

Copyright (c) 2012-2015 Denis Demidov <dennis.demidov@gmail.com>

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
 * \file   amgcl/amgcl.hpp
 * \author Denis Demidov <dennis.demidov@gmail.com>
 * \brief  Generic algebraic multigrid framework.
 */

#include <iostream>
#include <iomanip>
#include <list>

#include <boost/io/ios_state.hpp>
#include <boost/static_assert.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/foreach.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/static_assert.hpp>

#include <amgcl/backend/builtin.hpp>
#include <amgcl/solver/detail/default_inner_product.hpp>
#include <amgcl/util.hpp>

/// Primary namespace.
namespace amgcl {

/// Algebraic multigrid method.
/**
 * AMG is one the most effective methods for solution of large sparse
 * unstructured systems of equations, arising, for example, from discretization
 * of PDEs on unstructured grids \cite Trottenberg2001. The method can be used
 * as a black-box solver for various computational problems, since it does not
 * require any information about the underlying geometry.
 *
 * The three template parameters allow the user to select the exact components
 * of the method:
 *  1. *Backend* to transfer the constructed hierarchy to,
 *  2. *Coarsening* strategy for hierarchy construction, and
 *  3. *Relaxation* scheme (smoother to use during the solution phase).
 *
 * Instance of the class builds the AMG hierarchy for the given system matrix
 * and is intended to be used as a preconditioner.
 */
template <
    class Backend,
    class Coarsening,
    template <class> class Relax
    >
class amg {
    public:
        typedef Backend backend_type;

        typedef typename Backend::value_type value_type;
        typedef typename Backend::matrix     matrix;
        typedef typename Backend::vector     vector;
        typedef Relax<Backend>               relax_type;

        /// Backend parameters.
        typedef typename Backend::params     backend_params;

        /// Parameters of the method.
        /**
         * The amgcl::amg::params struct includes parameters for each
         * component of the method as well as some universal parameters.
         */
        struct params {
            typedef typename Coarsening::params coarsening_params;
            typedef typename relax_type::params relax_params;

            coarsening_params coarsening;   ///< Coarsening parameters.
            relax_params      relax;        ///< Relaxation parameters.

            /// Specifies when level is coarse enough to be solved directly.
            /**
             * If number of variables at a next level in the hierarchy becomes
             * lower than this threshold, then the hierarchy construction is
             * stopped and the linear system is solved directly at this level.
             */
            unsigned coarse_enough;

            /// Number of pre-relaxations.
            unsigned npre;

            /// Number of post-relaxations.
            unsigned npost;

            /// Number of cycles (1 for V-cycle, 2 for W-cycle, etc.).
            unsigned ncycle;

            /// Number of cycles to make as part of preconditioning.
            unsigned pre_cycles;

            params() :
                coarse_enough( Backend::direct_solver::coarse_enough() ),
                npre         (   1 ),
                npost        (   1 ),
                ncycle       (   1 ),
                pre_cycles   (   1 )
            {}

            params(const boost::property_tree::ptree &p)
                : AMGCL_PARAMS_IMPORT_CHILD(p, coarsening),
                  AMGCL_PARAMS_IMPORT_CHILD(p, relax),
                  AMGCL_PARAMS_IMPORT_VALUE(p, coarse_enough),
                  AMGCL_PARAMS_IMPORT_VALUE(p, npre),
                  AMGCL_PARAMS_IMPORT_VALUE(p, npost),
                  AMGCL_PARAMS_IMPORT_VALUE(p, ncycle),
                  AMGCL_PARAMS_IMPORT_VALUE(p, pre_cycles)
            {}

            void get(
                    boost::property_tree::ptree &p,
                    const std::string &path = ""
                    ) const
            {
                AMGCL_PARAMS_EXPORT_CHILD(p, path, coarsening);
                AMGCL_PARAMS_EXPORT_CHILD(p, path, relax);
                AMGCL_PARAMS_EXPORT_VALUE(p, path, coarse_enough);
                AMGCL_PARAMS_EXPORT_VALUE(p, path, npre);
                AMGCL_PARAMS_EXPORT_VALUE(p, path, npost);
                AMGCL_PARAMS_EXPORT_VALUE(p, path, ncycle);
                AMGCL_PARAMS_EXPORT_VALUE(p, path, pre_cycles);
            }
        } prm;

        /// Builds the AMG hierarchy for the system matrix.
        /**
         * The input matrix is copied here and is safe to delete afterwards.
         *
         * \param A The system matrix. Should be convertible to
         *          amgcl::backend::crs<>.
         * \param p AMG parameters.
         *
         * \sa amgcl/adapter/crs_tuple.hpp
         */
        template <class Matrix>
        amg(
                const Matrix &M,
                const params &p = params(),
                const backend_params &bprm = backend_params()
           ) : prm(p)
        {
            precondition(
                    backend::rows(M) == backend::cols(M),
                    "Matrix should be square!"
                    );

            boost::shared_ptr<build_matrix> P, R;
            boost::shared_ptr<build_matrix> A = boost::make_shared<build_matrix>( M );
            sort_rows(*A);

            std::cout
                << "Matrix size: "
                << (
                        sizeof(size_t) * backend::rows(*A) +
                        sizeof(size_t) * backend::nonzeros(*A) +
                        sizeof(value_type) * backend::nonzeros(*A)
                   ) / (1024.0 * 1024.0)
                << std::endl;

            while( backend::rows(*A) > prm.coarse_enough) {
                TIC("transfer operators");
                boost::tie(P, R) = Coarsening::transfer_operators(
                        *A, prm.coarsening);
                precondition(
                        backend::cols(*P) > 0,
                        "Zero-sized coarse level in amgcl (diagonal matrix?)"
                        );
                TOC("transfer operators");

                TIC("move to backend")
                levels.push_back( level(A, P, R, prm, bprm) );
                TOC("move to backend")

                TIC("coarse operator");
                A = Coarsening::coarse_operator(*A, *P, *R, prm.coarsening);
                sort_rows(*A);
                TOC("coarse operator");
            }

            TIC("coarsest level");
            levels.push_back( level(A, bprm, levels.empty()) );
            TOC("coarsest level");
        }

        /// Performs single V-cycle for the given right-hand side and solution.
        /**
         * \param rhs Right-hand side vector.
         * \param x   Solution vector.
         */
        template <class Vec1, class Vec2>
        void cycle(
                const Vec1 &rhs,
#ifdef BOOST_NO_CXX11_RVALUE_REFERENCES
                Vec2       &x
#else
                Vec2       &&x
#endif
                ) const
        {
            cycle(levels.begin(), rhs, x);
        }

        /// Performs single V-cycle after clearing x.
        /**
         * This is intended for use as a preconditioning procedure.
         *
         * \param rhs Right-hand side vector.
         * \param x   Solution vector.
         */
        template <class Vec1, class Vec2>
        void apply(
                const Vec1 &rhs,
#ifdef BOOST_NO_CXX11_RVALUE_REFERENCES
                Vec2       &x
#else
                Vec2       &&x
#endif
                ) const
        {
            if (prm.pre_cycles) {
                backend::clear(x);
                for(unsigned i = 0; i < prm.pre_cycles; ++i)
                    cycle(levels.begin(), rhs, x);
            } else {
                backend::copy(rhs, x);
            }
        }

        /// Returns the system matrix from the finest level.
        const matrix& system_matrix() const {
            return *levels.front().A;
        }

    private:
        typedef typename backend::builtin<value_type>::matrix build_matrix;

        struct level {
            boost::shared_ptr<matrix> A;
            boost::shared_ptr<matrix> P;
            boost::shared_ptr<matrix> R;

            boost::shared_ptr<vector> f;
            boost::shared_ptr<vector> u;
            boost::shared_ptr<vector> t;

            boost::shared_ptr< typename Backend::direct_solver > solve;

            boost::shared_ptr<relax_type> relax;

            size_t m_rows, m_nonzeros;

            level(
                    boost::shared_ptr<build_matrix> a,
                    boost::shared_ptr<build_matrix> p,
                    boost::shared_ptr<build_matrix> r,
                    const params &prm,
                    const backend_params &bprm
                 ) :
                A( Backend::copy_matrix(a, bprm) ),
                P( Backend::copy_matrix(p, bprm) ),
                R( Backend::copy_matrix(r, bprm) ),
                f( Backend::create_vector(backend::rows(*a), bprm) ),
                u( Backend::create_vector(backend::rows(*a), bprm) ),
                t( Backend::create_vector(backend::rows(*a), bprm) ),
                relax( new relax_type(*a, prm.relax, bprm) ),
                m_rows( backend::rows(*A) ),
                m_nonzeros( backend::nonzeros(*A) )
            { }

            level(
                    boost::shared_ptr<build_matrix> a,
                    const backend_params &bprm,
                    bool no_finer_levels
                 ) :
                f( Backend::create_vector(backend::rows(*a), bprm) ),
                u( Backend::create_vector(backend::rows(*a), bprm) ),
                solve( Backend::create_solver(a, bprm) ),
                m_rows( backend::rows(*a) ),
                m_nonzeros( backend::nonzeros(*a) )
            {
                if (no_finer_levels)
                    A = Backend::copy_matrix(a, bprm);
            }

            size_t rows() const {
                return m_rows;
            }

            size_t nonzeros() const {
                return m_nonzeros;
            }

            size_t bytes() const {
                size_t nbytes = 0;
                nbytes += sizeof(value_type) * rows() * 3;
                if (A) {
                    nbytes += sizeof(size_t) * rows();
                    nbytes += sizeof(value_type) * backend::nonzeros(*A);
                    nbytes += sizeof(size_t) * backend::nonzeros(*A);
                }
                if (R) {
                    nbytes += sizeof(size_t) * rows();
                    nbytes += sizeof(value_type) * backend::nonzeros(*R);
                    nbytes += sizeof(size_t) * backend::nonzeros(*R);
                }
                if (P) {
                    nbytes += sizeof(size_t) * rows();
                    nbytes += sizeof(value_type) * backend::nonzeros(*P);
                    nbytes += sizeof(size_t) * backend::nonzeros(*P);
                }
                return nbytes;
            }
        };

        typedef typename std::list<level>::const_iterator level_iterator;

        std::list<level> levels;

        template <class Vec1, class Vec2>
        void cycle(level_iterator lvl, const Vec1 &rhs, Vec2 &x) const
        {
            level_iterator nxt = lvl; ++nxt;

            if (nxt == levels.end()) {
                TIC("coarse");
                (*lvl->solve)(rhs, x);
                TOC("coarse");
            } else {
                for (size_t j = 0; j < prm.ncycle; ++j) {
                    TIC("relax");
                    for(size_t i = 0; i < prm.npre; ++i)
                        lvl->relax->apply_pre(*lvl->A, rhs, x, *lvl->t, prm.relax);
                    TOC("relax");

                    TIC("residual");
                    backend::residual(rhs, *lvl->A, x, *lvl->t);
                    TOC("residual");

                    TIC("restrict");
                    backend::spmv(1, *lvl->R, *lvl->t, 0, *nxt->f);
                    TOC("restrict");

                    backend::clear(*nxt->u);
                    cycle(nxt, *nxt->f, *nxt->u);

                    TIC("prolongate");
                    backend::spmv(1, *lvl->P, *nxt->u, 1, x);
                    TOC("prolongate");

                    TIC("relax");
                    for(size_t i = 0; i < prm.npre; ++i)
                        lvl->relax->apply_post(*lvl->A, rhs, x, *lvl->t, prm.relax);
                    TOC("relax");
                }
            }
        }

    template <class B, class C, template <class> class R>
    friend std::ostream& operator<<(std::ostream &os, const amg<B, C, R> &a);
};

/// Sends information about the AMG hierarchy to output stream.
template <class B, class C, template <class> class R>
std::ostream& operator<<(std::ostream &os, const amg<B, C, R> &a)
{
    typedef typename amg<B, C, R>::level level;
    boost::io::ios_all_saver stream_state(os);

    size_t sum_dof = 0;
    size_t sum_nnz = 0;

    BOOST_FOREACH(const level &lvl, a.levels) {
        sum_dof += lvl.rows();
        sum_nnz += lvl.nonzeros();
    }

    os << "Number of levels:    "   << a.levels.size()
        << "\nOperator complexity: " << std::fixed << std::setprecision(2)
        << 1.0 * sum_nnz / a.levels.front().nonzeros()
        << "\nGrid complexity:     " << std::fixed << std::setprecision(2)
        << 1.0 * sum_dof / a.levels.front().rows()
        << "\n\nlevel     unknowns       nonzeros\n"
        << "---------------------------------\n";

    size_t depth = 0;
    BOOST_FOREACH(const level &lvl, a.levels) {
        os << std::setw(5)  << depth++
            << std::setw(13) << lvl.rows()
            << std::setw(15) << lvl.nonzeros() << " ("
            << std::setw(5) << std::fixed << std::setprecision(2)
            << 100.0 * lvl.nonzeros() / sum_nnz
            << "%)"
            << " " << lvl.bytes() / (1024.0 * 1024.0)
            << std::endl;
    }

    return os;
}

} // namespace amgcl

#endif
