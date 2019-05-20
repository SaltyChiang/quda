#pragma once

#include <quda.h>
#include <quda_internal.h>
#include <dirac_quda.h>
#include <color_spinor_field.h>

namespace quda
{

  class EigenSolver
  {

protected:
    QudaEigParam *eig_param;
    TimeProfile profile;

    // Timings for components of the eigensolver
    //-----------------------------------------
    double time_;
    double time_e;   // time in Eigen
    double time_mv;  // time in matVec
    double time_mb;  // time in multiblas
    double time_svd; // time to compute SVD

    // Problem parameters
    //------------------
    int nEv;        // Size of initial factorisation
    int nKr;        // Size of Krylov space after extension
    int nConv;      // Number of converged eigenvalues requested
    double tol;     // Tolerance on eigenvalues
    bool reverse;   // True if using polynomial acceleration
    char *spectrum; // Part of the spectrum to be computed.

    // Algorithm variables
    //--------------------
    bool converged;
    int restart_iter;
    int max_restarts;
    int check_interval;
    int iter;
    int iter_converged;
    int iter_locked;
    int iter_keep;
    int num_converged;
    int num_locked;
    int num_keep;
    
    double *residua;

    // Device side vector workspace
    std::vector<ColorSpinorField *> r;
    std::vector<ColorSpinorField *> d_vecs_tmp;

    Complex *Qmat;

public:
    EigenSolver(QudaEigParam *eig_param, TimeProfile &profile);
    virtual ~EigenSolver();

    virtual void operator()(std::vector<ColorSpinorField *> &kSpace, std::vector<Complex> &evals) = 0;

    static EigenSolver *create(QudaEigParam *eig_param, const Dirac &mat, TimeProfile &profile);

    /**
       @brief Applies the specified matVec operation:
       M, Mdag, MMdag, MdagM
       @param[in] mat Matrix operator
       @param[in] out Output spinor
       @param[in] in Input spinor
    */

    void matVec(const Dirac &mat, ColorSpinorField &out, const ColorSpinorField &in);

    /**
       @brief Promoted the specified matVec operation:
       M, Mdag, MMdag, MdagM to a Chebyshev polynomial
       @param[in] mat Matrix operator
       @param[in] out Output spinor
       @param[in] in Input spinor
    */
    void chebyOp(const Dirac &mat, ColorSpinorField &out, const ColorSpinorField &in);

    /**
       @brief Orthogonalise input vector r against
       vector space v
       @param[out] Sum of inner products
       @param[in] v Vector space
       @param[in] r Vector to be orthogonalised
       @param[in] j Number of vectors in v to orthogonalise against
    */
    Complex orthogonalise(std::vector<ColorSpinorField *> v, std::vector<ColorSpinorField *> r, int j);

    /**
       @brief Orthogonalise input vector r against
       vector space v using block-BLAS
       @param[out] Sum of inner products
       @param[in] v Vector space
       @param[in] r Vector to be orthogonalised
       @param[in] j Number of vectors in v to orthogonalise against
    */
    Complex blockOrthogonalise(std::vector<ColorSpinorField *> v, std::vector<ColorSpinorField *> r, int j);

    /**
       @brief Deflate vector with Eigenvectors
       @param[in] vec_defl The deflated vector
       @param[in] vec The input vector
       @param[in] evecs The eigenvectors to use in deflation
       @param[in] evals The eigenvalues to use in deflation
    */
    void deflate(std::vector<ColorSpinorField *> vec_defl, std::vector<ColorSpinorField *> vec,
                 std::vector<ColorSpinorField *> evecs, std::vector<Complex> evals);

    /**
       @brief Compute eigenvalues and their residiua
       @param[in] mat Matrix operator
       @param[in] evecs The eigenvectors
       @param[in] evals The eigenvalues
       @param[in] k The number to compute
    */
    void computeEvals(const Dirac &mat, std::vector<ColorSpinorField *> &evecs, std::vector<Complex> &evals, int k);
    
    /**
       @brief Load vectors from file
       @param[in] eig_vecs The eigenvectors to load
       @param[in] file The filename to load
    */
    void loadVectors(std::vector<ColorSpinorField *> &eig_vecs, std::string file);

    /**
       @brief Save vectors to file
       @param[in] eig_vecs The eigenvectors to save
       @param[in] file The filename to save
    */
    void saveVectors(std::vector<ColorSpinorField *> &eig_vecs, std::string file);

    /**
       @brief Load and check eigenpairs from file
       @param[in] mat Matrix operator
       @param[in] eig_vecs The eigenvectors to save
       @param[in] file The filename to save
    */
    void loadFromFile(const Dirac &mat, std::vector<ColorSpinorField *> &eig_vecs, std::vector<Complex> &evals);
  };

  /**
     @brief Thick Restarted LAnczos Method.
  */
  class TRLM : public EigenSolver {
    
  public:
    const Dirac &mat;
    TRLM(QudaEigParam *eig_param, const Dirac &mat, TimeProfile &profile);
    virtual ~TRLM();
    
    //Variable size
    std::vector<double> ritz_mat;
    
    // Tridiagonal/Arrow matrix, fixed size.
    double *alpha;
    double *beta;
    
    //Used to clone vectors and resize arrays.
    ColorSpinorParam csParam;
    
    /**
       @brief Compute eigenpairs
       @param[in] kSpace Krylov vector space
       @param[in] evals Computed eigenvalues
       
    */
    void operator()(std::vector<ColorSpinorField*> &kSpace,
		    std::vector<Complex> &evals);

    /**
       @brief Lanczos step: extends the Kylov space.
       @param[in] v Vector space
       @param[in] j Index of vector being computed
    */
    void lanczosStep(std::vector<ColorSpinorField *> v, int j);

    /**
       @brief Reorder the Krylov space by eigenvalue
       @param[in] kSpace the Krylov space
    */
    void reorder(std::vector<ColorSpinorField*> &kSpace);

    /**
       @brief Get the eigendecomposition from the arrow matrix
       @param[in] nLocked Number of locked eigenvectors
       @param[in] arrow_pos position of arrowhead
    */
    void eigensolveFromArrowMat(int nLocked, int arror_pos);

    /**
       @brief Get the eigen-decomposition from the arrow matrix
       @param[in] nKspace current Kryloc space
    */
    void computeKeptRitz(std::vector<ColorSpinorField*> &kSpace);

    /**
       @brief Computes Left/Right SVD from pre computed Right/Left
       @param[in] v Vector space
       @param[in] r Current vector to add
       @param[in] kSpace
       @param[in] evecs Computed eigenvectors of NormOp
       @param[in] evals Computed eigenvalues of NormOp
       @param[in] inverse Inverse sort if using PolyAcc
    */
    void computeSVD(std::vector<ColorSpinorField *> &kSpace, std::vector<ColorSpinorField *> &evecs,
                    std::vector<Complex> &evals, bool reverse);
  };
  
  /**
     @brief Computes eigen-decomposition using QUDA's arpack interface 
     @param[in] h_evecs host pointer to evecs
     @param[in] h_evals host pointer to evals
     @param[in] mat The operator
     @param[in] eig_param parameter structure for all QUDA eigensolvers
     @param[in] cpuParam parameter structure for creating device vectors
  */
  void arpack_solve(void *h_evecs, void *h_evals, const Dirac &mat, QudaEigParam *eig_param, ColorSpinorParam *cpuParam);

} // namespace quda