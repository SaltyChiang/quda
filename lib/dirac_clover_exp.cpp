#include <dirac_quda.h>
#include <blas_quda.h>
#include <multigrid.h>

namespace quda {

  DiracCloverExp::DiracCloverExp(const DiracParam &param) :
    DiracClover(param), degree(param.degreeExp), mass(param.mass)
  {
    if (clover->degreeExp <= 0) {
      errorQuda("Invalid source exponential expansion degree, which is %d",
          clover->degreeExp);
    }
    if (clover->degreeExp != degree) {
      if (clover->degreeExp == 1) {
        if (degree >= 0) {
          cloverExponential(*clover, degree, mass, false);
          clover->degreeExp = degree;
        } else {
          errorQuda("Invalid target exponential expansion degree, which is %d",
            degree);
        }
      } else {
        errorQuda("Invalid source exponential expansion degree to update clover, which is %d",
          clover->degreeExp);
      }
    }
  }

  DiracCloverExp::DiracCloverExp(const DiracCloverExp &dirac) :
    DiracClover(dirac), degree(dirac.degree), mass(dirac.mass) {}

  DiracCloverExp::~DiracCloverExp() { }

  DiracCloverExp& DiracCloverExp::operator=(const DiracCloverExp &dirac)
  {
    if (&dirac != this) {
      DiracClover::operator=(dirac);
      degree = dirac.degree;
      mass = dirac.mass;
    }
    return *this;
  }

  /*******
   * DiracCloverExpPC Starts here
   *******/
  DiracCloverExpPC::DiracCloverExpPC(const DiracParam &param) : 
    DiracCloverExp(param)
  {
    // For the preconditioned operator, we need to check that the inverse of the clover term is present
    cloverInvert(*clover, false);
  }

  DiracCloverExpPC::DiracCloverExpPC(const DiracCloverExpPC &dirac) : DiracCloverExp(dirac) { }

  DiracCloverExpPC::~DiracCloverExpPC() { }

  DiracCloverExpPC& DiracCloverExpPC::operator=(const DiracCloverExpPC &dirac)
  {
    if (&dirac != this) {
      DiracCloverExp::operator=(dirac);
    }
    return *this;
  }

  // Public method
  void DiracCloverExpPC::CloverInv(ColorSpinorField &out, const ColorSpinorField &in, 
				const QudaParity parity) const
  {
    checkParitySpinor(in, out);

    ApplyClover(out, in, *clover, true, parity);
    flops += 504ll*in.Volume();
  }

  // apply hopping term, then clover: (A_ee^-1 D_eo) or (A_oo^-1 D_oe),
  // and likewise for dagger: (A_ee^-1 D^dagger_eo) or (A_oo^-1 D^dagger_oe)
  // NOTE - this isn't Dslash dagger since order should be reversed!
  void DiracCloverExpPC::Dslash(ColorSpinorField &out, const ColorSpinorField &in, 
			     const QudaParity parity) const
  {
    checkParitySpinor(in, out);
    checkSpinorAlias(in, out);

    ApplyWilsonCloverPreconditioned(out, in, *gauge, *clover, 0.0, in, parity, dagger, commDim, profile);
    flops += 1824ll*in.Volume();
  }

  // xpay version of the above
  void DiracCloverExpPC::DslashXpay(ColorSpinorField &out, const ColorSpinorField &in, 
				 const QudaParity parity, const ColorSpinorField &x,
				 const double &k) const
  {
    checkParitySpinor(in, out);
    checkSpinorAlias(in, out);

    ApplyWilsonCloverPreconditioned(out, in, *gauge, *clover, k, x, parity, dagger, commDim, profile);
    flops += 1872ll*in.Volume();
  }

  // Apply the even-odd preconditioned clover-improved Dirac operator
  void DiracCloverExpPC::M(ColorSpinorField &out, const ColorSpinorField &in) const
  {
    double kappa2 = -kappa*kappa;
    bool reset1 = newTmp(&tmp1, in);

    bool symmetric =(matpcType == QUDA_MATPC_EVEN_EVEN || matpcType == QUDA_MATPC_ODD_ODD) ? true : false;
    int odd_bit = (matpcType == QUDA_MATPC_ODD_ODD || matpcType == QUDA_MATPC_ODD_ODD_ASYMMETRIC) ? 1 : 0;
    QudaParity parity[2] = {static_cast<QudaParity>((1 + odd_bit) % 2), static_cast<QudaParity>((0 + odd_bit) % 2)};

    if (!symmetric) {

      // No need to change order of calls for dagger
      // because the asymmetric operator is actually symmetric
      // A_oo -D_oe A^{-1}_ee D_eo -> A_oo -D^\dag_oe A^{-1}_ee D^\dag_eo
      // the pieces in Dslash and DslashXPay respect the dagger

      // DiracCloverExpPC::Dslash applies A^{-1}Dslash
      Dslash(*tmp1, in, parity[0]);
      // DiracClover::DslashXpay applies (A - kappa^2 D)
      DiracClover::DslashXpay(out, *tmp1, parity[1], in, kappa2);
    } else if (!dagger) { // symmetric preconditioning
      // We need two cases because M = 1-ADAD and M^\dag = 1-D^\dag A D^dag A
      // where A is actually a clover inverse.

      // This is the non-dag case: AD
      Dslash(*tmp1, in, parity[0]);

      // Then x + AD (AD)
      DslashXpay(out, *tmp1, parity[1], in, kappa2);
    } else { // symmetric preconditioning, dagger

      // This is the dagger: 1 - DADA
      //  i) Apply A
      CloverInv(out, in, parity[1]);
      // ii) Apply A D => ADA
      Dslash(*tmp1, out, parity[0]);
      // iii) Apply  x + D(ADA)
      DiracWilson::DslashXpay(out, *tmp1, parity[1], in, kappa2);
    }

    deleteTmp(&tmp1, reset1);
  }

  void DiracCloverExpPC::MdagM(ColorSpinorField &out, const ColorSpinorField &in) const
  {
    // need extra temporary because of symmetric preconditioning dagger
    // and for multi-gpu the input and output fields cannot alias
    bool reset = newTmp(&tmp2, in);
    M(*tmp2, in);
    Mdag(out, *tmp2);
    deleteTmp(&tmp2, reset);
  }

  void DiracCloverExpPC::prepare(ColorSpinorField* &src, ColorSpinorField* &sol, 
			      ColorSpinorField &x, ColorSpinorField &b, 
			      const QudaSolutionType solType) const
  {
    // we desire solution to preconditioned system
    if (solType == QUDA_MATPC_SOLUTION || solType == QUDA_MATPCDAG_MATPC_SOLUTION) {
      src = &b;
      sol = &x;
      return;
    }

    bool reset = newTmp(&tmp1, b.Even());
  
    // we desire solution to full system
    if (matpcType == QUDA_MATPC_EVEN_EVEN) {
      // src = A_ee^-1 (b_e + k D_eo A_oo^-1 b_o)
      src = &(x.Odd());
      CloverInv(*src, b.Odd(), QUDA_ODD_PARITY);
      DiracWilson::DslashXpay(*tmp1, *src, QUDA_EVEN_PARITY, b.Even(), kappa);
      CloverInv(*src, *tmp1, QUDA_EVEN_PARITY);
      sol = &(x.Even());
    } else if (matpcType == QUDA_MATPC_ODD_ODD) {
      // src = A_oo^-1 (b_o + k D_oe A_ee^-1 b_e)
      src = &(x.Even());
      CloverInv(*src, b.Even(), QUDA_EVEN_PARITY);
      DiracWilson::DslashXpay(*tmp1, *src, QUDA_ODD_PARITY, b.Odd(), kappa);
      CloverInv(*src, *tmp1, QUDA_ODD_PARITY);
      sol = &(x.Odd());
    } else if (matpcType == QUDA_MATPC_EVEN_EVEN_ASYMMETRIC) {
      // src = b_e + k D_eo A_oo^-1 b_o
      src = &(x.Odd());
      CloverInv(*tmp1, b.Odd(), QUDA_ODD_PARITY); // safe even when *tmp1 = b.odd
      DiracWilson::DslashXpay(*src, *tmp1, QUDA_EVEN_PARITY, b.Even(), kappa);
      sol = &(x.Even());
    } else if (matpcType == QUDA_MATPC_ODD_ODD_ASYMMETRIC) {
      // src = b_o + k D_oe A_ee^-1 b_e
      src = &(x.Even());
      CloverInv(*tmp1, b.Even(), QUDA_EVEN_PARITY); // safe even when *tmp1 = b.even
      DiracWilson::DslashXpay(*src, *tmp1, QUDA_ODD_PARITY, b.Odd(), kappa);
      sol = &(x.Odd());
    } else {
      errorQuda("MatPCType %d not valid for DiracCloverExpPC", matpcType);
    }

    // here we use final solution to store parity solution and parity source
    // b is now up for grabs if we want

    deleteTmp(&tmp1, reset);

  }

  void DiracCloverExpPC::reconstruct(ColorSpinorField &x, const ColorSpinorField &b,
				  const QudaSolutionType solType) const
  {
    if (solType == QUDA_MATPC_SOLUTION || solType == QUDA_MATPCDAG_MATPC_SOLUTION) {
      return;
    }

    checkFullSpinor(x, b);

    bool reset = newTmp(&tmp1, b.Even());

    // create full solution

    if (matpcType == QUDA_MATPC_EVEN_EVEN ||
	matpcType == QUDA_MATPC_EVEN_EVEN_ASYMMETRIC) {
      // x_o = A_oo^-1 (b_o + k D_oe x_e)
      DiracWilson::DslashXpay(*tmp1, x.Even(), QUDA_ODD_PARITY, b.Odd(), kappa);
      CloverInv(x.Odd(), *tmp1, QUDA_ODD_PARITY);
    } else if (matpcType == QUDA_MATPC_ODD_ODD ||
	       matpcType == QUDA_MATPC_ODD_ODD_ASYMMETRIC) {
      // x_e = A_ee^-1 (b_e + k D_eo x_o)
      DiracWilson::DslashXpay(*tmp1, x.Odd(), QUDA_EVEN_PARITY, b.Even(), kappa);
      CloverInv(x.Even(), *tmp1, QUDA_EVEN_PARITY);
    } else {
      errorQuda("MatPCType %d not valid for DiracCloverExpPC", matpcType);
    }

    deleteTmp(&tmp1, reset);

  }

  void DiracCloverExpPC::createCoarseOp(GaugeField &Y, GaugeField &X, const Transfer &T,
				     double kappa, double mass, double mu, double mu_factor) const {
    double a = - 2.0 * kappa * mu * T.Vectors().TwistFlavor();
    CoarseOp(Y, X, T, *gauge, clover, kappa, a, -mu_factor, QUDA_CLOVERPC_DIRAC, matpcType);
  }

  void DiracCloverExpPC::prefetch(QudaFieldLocation mem_space, qudaStream_t stream) const
  {
    Dirac::prefetch(mem_space, stream);

    bool symmetric = (matpcType == QUDA_MATPC_EVEN_EVEN || matpcType == QUDA_MATPC_ODD_ODD) ? true : false;
    int odd_bit = (matpcType == QUDA_MATPC_ODD_ODD || matpcType == QUDA_MATPC_ODD_ODD_ASYMMETRIC) ? 1 : 0;
    QudaParity parity[2] = {static_cast<QudaParity>((1 + odd_bit) % 2), static_cast<QudaParity>((0 + odd_bit) % 2)};

    if (symmetric) {
      clover->prefetch(mem_space, stream, CloverPrefetchType::INVERSE_CLOVER_PREFETCH_TYPE);
    } else {
      clover->prefetch(mem_space, stream, CloverPrefetchType::INVERSE_CLOVER_PREFETCH_TYPE, parity[0]);
      clover->prefetch(mem_space, stream, CloverPrefetchType::CLOVER_CLOVER_PREFETCH_TYPE, parity[1]);
    }
  }

} // namespace quda
