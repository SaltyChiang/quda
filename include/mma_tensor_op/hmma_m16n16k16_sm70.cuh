#pragma once

#include <mma.h>

// #define USE_FP16_HMMA_ACCUMULATE

constexpr QudaPrecision accumulate_precision()
{
#ifdef USE_FP16_HMMA_ACCUMULATE
  return QUDA_HALF_PRECISION;
#else
  return QUDA_SINGLE_PRECISION;
#endif
}

namespace quda
{
  namespace mma
  {
    __device__ __host__ constexpr int inline pad_size(int m) { return m == 48 ? 2 : 10; }

    constexpr int WMMA_M = 16;
    constexpr int WMMA_N = 16;
    constexpr int WMMA_K = 4;

    constexpr int warp_size = 32;

    struct WarpRegisterMapping {

      int quad_id;
      int quad_row;
      int quad_col;
      int quad_hilo;   // quad higher or lower.
      int quad_thread; // 0,1,2,3

      __device__ inline WarpRegisterMapping(int thread_id)
      {
        const int lane_id = thread_id & 31;
        const int octl_id = lane_id >> 2;
        quad_id = octl_id & 3;
        quad_row = quad_id & 1;
        quad_col = quad_id >> 1;
        quad_hilo = (octl_id >> 2) & 1;
        quad_thread = lane_id & 3;
      }
    };

    template <int stride> struct MmaOperandA {

      unsigned reg[2];

      __device__ inline void load(void *smem, int k, int warp_row, const WarpRegisterMapping &wrm)
      {
        unsigned *A = reinterpret_cast<unsigned *>(smem);
        const int idx_strided = k * 4 + wrm.quad_thread;
        const int idx_contiguous = warp_row * 8 + wrm.quad_row * 4 + wrm.quad_hilo * 2;
        const int thread_offset_a = idx_strided * stride + idx_contiguous;
        reg[0] = A[thread_offset_a + 0];
        reg[1] = A[thread_offset_a + 1];
      }

      __device__ inline void negate()
      {
        asm volatile("neg.f16x2 %0, %0;" : "+r"(reg[0]));
        asm volatile("neg.f16x2 %0, %0;" : "+r"(reg[1]));
      }
    };

    template <int stride> struct MmaOperandB {

      unsigned reg[2];

      __device__ inline void load(void *smem, int k, int warp_col, const WarpRegisterMapping &wrm)
      {
        unsigned *B = reinterpret_cast<unsigned *>(smem);
        const int idx_strided = k * 4 + wrm.quad_thread;
        const int idx_contiguous = warp_col * 8 + wrm.quad_col * 4 + wrm.quad_hilo * 2;
        const int thread_offset_b = idx_strided * stride + idx_contiguous;
        reg[0] = B[thread_offset_b + 0];
        reg[1] = B[thread_offset_b + 1];
      }
    };

    template <int stride, class store_type> struct MmaOperandC {
    };

    template <int stride> struct MmaOperandC<stride, half> {

      using reg_type = unsigned;
      reg_type reg[4];

      __device__ inline MmaOperandC()
      {
#pragma unroll
        for (int i = 0; i < 4; i++) { reg[i] = 0; }
      }

      __device__ inline void store(void *smem, int warp_row, int warp_col, const WarpRegisterMapping &wrm)
      {
        reg_type *C = reinterpret_cast<reg_type *>(smem);

        const int idx_strided = warp_row * 16 + wrm.quad_row * 8 + wrm.quad_hilo * 4 + wrm.quad_thread;
        const int idx_contiguous = warp_col * 8 + wrm.quad_col * 4;
        const int thread_offset_c = idx_strided * stride + idx_contiguous;
#pragma unroll
        for (int i = 0; i < 4; i++) { C[thread_offset_c + i] = reg[i]; }
      }

      template <class F> __device__ inline void abs_max(F &max)
      {
#pragma unroll
        for (int i = 0; i < 4; i++) {
          const half2 h2 = __habs2(*(reinterpret_cast<const half2 *>(&(reg[i]))));
          max = fmax(max, h2.x);
          max = fmax(max, h2.y);
        }
      }
    };

    template <int stride> struct MmaOperandC<stride, float> {

      using reg_type = float;
      reg_type reg[8];

      __device__ inline MmaOperandC()
      {
#pragma unroll
        for (int i = 0; i < 8; i++) { reg[i] = 0; }
      }

      __device__ inline void store(void *smem, int warp_row, int warp_col, const WarpRegisterMapping &wrm)
      {
        half2 *C = reinterpret_cast<half2 *>(smem);

        const int idx_strided = warp_row * 16 + wrm.quad_row * 8 + wrm.quad_hilo * 4 + (wrm.quad_thread % 2);
        const int idx_contiguous = warp_col * 8 + wrm.quad_col * 4 + (wrm.quad_thread / 2);

        int thread_offset_c = idx_strided * stride + idx_contiguous;
        C[thread_offset_c] = __floats2half2_rn(reg[0], reg[1]);

        thread_offset_c = (idx_strided + 2) * stride + idx_contiguous;
        C[thread_offset_c] = __floats2half2_rn(reg[2], reg[3]);

        thread_offset_c = idx_strided * stride + (idx_contiguous + 2);
        C[thread_offset_c] = __floats2half2_rn(reg[4], reg[5]);

        thread_offset_c = (idx_strided + 2) * stride + (idx_contiguous + 2);
        C[thread_offset_c] = __floats2half2_rn(reg[6], reg[7]);
      }

      template <class F> __device__ inline void abs_max(F &max)
      {
#pragma unroll
        for (int i = 0; i < 8; i++) { max = fmax(max, fabsf(reg[i])); }
      }
    };

    template <class TA, class TB, class TC> __device__ inline void gemm(const TA &op_a, const TB &op_b, TC &op_c)
    {
#ifdef USE_FP16_HMMA_ACCUMULATE
      asm volatile("mma.sync.aligned.m8n8k4.col.row.f16.f16.f16.f16 {%0,%1,%2,%3}, {%4,%5}, {%6,%7}, {%0,%1,%2,%3};"
                   : "+r"(op_c.reg[0]), "+r"(op_c.reg[1]), "+r"(op_c.reg[2]), "+r"(op_c.reg[3])
                   : "r"(op_a.reg[0]), "r"(op_a.reg[1]), "r"(op_b.reg[0]), "r"(op_b.reg[1]));
#else
      asm volatile("mma.sync.aligned.m8n8k4.col.row.f32.f16.f16.f32 {%0,%1,%2,%3,%4,%5,%6,%7}, {%8,%9}, {%10,%11}, "
                   "{%0,%1,%2,%3,%4,%5,%6,%7};"
                   : "+f"(op_c.reg[0]), "+f"(op_c.reg[1]), "+f"(op_c.reg[2]), "+f"(op_c.reg[3]), "+f"(op_c.reg[4]),
                     "+f"(op_c.reg[5]), "+f"(op_c.reg[6]), "+f"(op_c.reg[7])
                   : "r"(op_a.reg[0]), "r"(op_a.reg[1]), "r"(op_b.reg[0]), "r"(op_b.reg[1]));
#endif
    }

    template <typename real, int length> struct Structure {
      real v[length];
      __host__ __device__ inline const real &operator[](int i) const { return v[i]; }
      __host__ __device__ inline real &operator[](int i) { return v[i]; }
    };

    template <int N, class TC, class GmemOperandC>
    inline __device__ void store_complex(int warp_row, int warp_col, WarpRegisterMapping wrm, GmemOperandC cc,
                                         TC op_c_real, TC op_c_imag)
    {

#ifdef USE_FP16_HMMA_ACCUMULATE
      const int row = warp_row + wrm.quad_row * 8 + wrm.quad_hilo * 4 + wrm.quad_thread;
      const int col = warp_col + wrm.quad_col * 8;

      constexpr bool fixed = GmemOperandC::fixed;
      using structure = typename std::conditional<fixed, Structure<short, 16>, Structure<float, 16>>::type;
      trove::coalesced_ptr<structure> ptr_(reinterpret_cast<structure *>(cc.data()));
      structure s;

#pragma unroll
      for (int i = 0; i < 4; i++) {
        const half2 r2 = *(reinterpret_cast<const half2 *>(&(op_c_real.reg[i])));
        const half2 i2 = *(reinterpret_cast<const half2 *>(&(op_c_imag.reg[i])));
        if (fixed) {
          const float scale = cc.scale;
          s[i * 4 + 0] = __half2short_rn(__half2float(r2.x) * scale);
          s[i * 4 + 1] = __half2short_rn(__half2float(i2.x) * scale);
          s[i * 4 + 2] = __half2short_rn(__half2float(r2.y) * scale);
          s[i * 4 + 3] = __half2short_rn(__half2float(i2.y) * scale);
        } else {
          s[i * 4 + 0] = __half2float(r2.x);
          s[i * 4 + 1] = __half2float(i2.x);
          s[i * 4 + 2] = __half2float(r2.y);
          s[i * 4 + 3] = __half2float(i2.y);
        }
      }

      ptr_[(row * N + col) / 8] = s;
#else // USE_FP16_HMMA_ACCUMULATE
      const int row = warp_row + wrm.quad_row * 8 + wrm.quad_hilo * 4 + (wrm.quad_thread % 2);
      const int col = warp_col + wrm.quad_col * 8 + (wrm.quad_thread / 2) * 2;

      constexpr bool fixed = GmemOperandC::fixed;
      using structure = typename std::conditional<fixed, Structure<short, 4>, Structure<float, 4>>::type;
      trove::coalesced_ptr<structure> ptr_(reinterpret_cast<structure *>(cc.data()));
      structure s;

#pragma unroll
      for (int i = 0; i < 4; i++) {
        const half2 r2 = *(reinterpret_cast<const half2 *>(&(op_c_real.reg[i])));
        const half2 i2 = *(reinterpret_cast<const half2 *>(&(op_c_imag.reg[i])));
        if (fixed) {
          const float scale = cc.scale;
          s[0] = short(op_c_real.reg[i * 2 + 0] * scale);
          s[1] = short(op_c_imag.reg[i * 2 + 0] * scale);
          s[2] = short(op_c_real.reg[i * 2 + 1] * scale);
          s[3] = short(op_c_imag.reg[i * 2 + 1] * scale);
        } else {
          s[0] = op_c_real.reg[i * 2 + 0];
          s[1] = op_c_imag.reg[i * 2 + 0];
          s[2] = op_c_real.reg[i * 2 + 1];
          s[3] = op_c_imag.reg[i * 2 + 1];
        }
        ptr_[((row + (i % 2) * 2) * N + (col + (i / 2) * 4)) / 2] = s;
      }
#endif
    }
  } // namespace mma
} // namespace quda
