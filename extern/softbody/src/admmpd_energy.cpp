// Copyright Matt Overby 2020.
// Distributed under the MIT License.

#include "admmpd_energy.h"
#include "admmpd_types.h"
#include "svd/ImplicitQRSVD.h"
#include <iostream>
#include <Eigen/Eigenvalues> 

namespace admmpd {
using namespace Eigen;

Lame::Lame()
{
	set_from_youngs_poisson(10000000,0.399);
}

void Lame::set_from_youngs_poisson(double youngs, double poisson)
{
	m_mu = youngs/(2.0*(1.0+poisson));
	m_lambda = youngs*poisson/((1.0+poisson)*(1.0-2.0*poisson));
	m_bulk_mod = m_lambda + (2.0/3.0)*m_mu;
}

void EnergyTerm::signed_svd(
	const Eigen::Matrix<double,3,3>& A, 
	Eigen::Matrix<double,3,3> &U, 
	Eigen::Matrix<double,3,1> &S, 
	Eigen::Matrix<double,3,3> &V)
{
    JIXIE::singularValueDecomposition(A,U,S,V);
	// Should replace this with 
//	JacobiSVD<Matrix3d> svd(A, ComputeFullU | ComputeFullV);
//	S = svd.singularValues();
//	U = svd.matrixU();
//	V = svd.matrixV();
//	Matrix3d J = Matrix3d::Identity();
//	J(2,2) = -1.0;
//	if (U.determinant() < 0.0)
//	{
//		U = U * J;
//		S[2] = -S[2];
//	}
//	if (V.determinant() < 0.0)
//	{
//		Matrix3d Vt = V.transpose();
//		Vt = J * Vt;
//		V = Vt.transpose();
//		S[2] = -S[2];
//	}
}

void EnergyTerm::update(
	const Options *options,
	int index,
	int energyterm_type,
	double rest_volume,
	double weight,
	const Eigen::MatrixXd *x,
	const Eigen::MatrixXd *Dx,
	Eigen::MatrixXd *z,
	Eigen::MatrixXd *u)
{
	switch (energyterm_type)
	{
		default: break;
		case ENERGYTERM_TET: {
			update_tet(options,index,rest_volume,weight,x,Dx,z,u);
		} break;
		case ENERGYTERM_TRIANGLE: {
			update_tri(options,index,rest_volume,weight,x,Dx,z,u);
		} break;
	}
} // end EnergyTerm::update

void EnergyTerm::update_tet(
	const Options *options,
	int index,
	double rest_volume,
	double weight,
	const Eigen::MatrixXd *x,
	const Eigen::MatrixXd *Dx,
	Eigen::MatrixXd *z,
	Eigen::MatrixXd *u)
{
	Lame lame;
	lame.set_from_youngs_poisson(options->youngs,options->poisson);

	(void)(x);
	Matrix3d Dix = Dx->block<3,3>(index,0);
	Matrix3d ui = u->block<3,3>(index,0);
	Matrix3d zi = Dix + ui;

	Matrix3d U, V;
	Vector3d S;
	signed_svd(zi, U, S, V);
	const Vector3d s0 = S;

	switch (options->elastic_material)
	{
		default:
		case ELASTIC_ARAP:
		{
			S = Vector3d::Ones();
			double k = lame.m_bulk_mod;
			double kv = k * rest_volume;
			double w2 = weight*weight;
			Matrix3d p = U * S.asDiagonal() * V.transpose();
			zi = (kv*p + w2*zi) / (w2 + kv);
		} break;
		case ELASTIC_NH:
		{
			solve_prox(options,index,lame,s0,S);
			zi = U * S.asDiagonal() * V.transpose();
		} break;
	}

	ui += (Dix - zi);
	u->block<3,3>(index,0) = ui;
	z->block<3,3>(index,0) = zi;

} // end EnergyTerm::update

void EnergyTerm::update_tri(
	const Options *options,
	int index,
	double rest_area,
	double weight,
	const Eigen::MatrixXd *x,
	const Eigen::MatrixXd *Dx,
	Eigen::MatrixXd *z,
	Eigen::MatrixXd *u)
{
	Lame lame;
	lame.set_from_youngs_poisson(options->youngs,options->poisson);
	Vector2d limit = options->strain_limit;
	limit[0] = std::min(limit[0], 1.0);
	limit[1] = std::max(limit[1], 1.0);

	(void)(options);
	typedef Matrix<double,2,3> Matrix23d;
	// For we'll assume ARAP energy. If we need nonlinear energies
	// in the future that's totally doable.
	(void)(x);
	Matrix23d Dix = Dx->block<2,3>(index,0);
	Matrix23d ui = u->block<2,3>(index,0);
	Matrix<double,3,2> zi_T = (Dix + ui).transpose();
	JacobiSVD<Matrix<double,3,2> > svd(zi_T, ComputeFullU | ComputeFullV);

	Matrix<double,3,2> S = Matrix<double,3,2>::Zero();
	S(0,0)=1; S(1,1)=1;
	Matrix<double,3,2> p = svd.matrixU() * S * svd.matrixV().transpose();
	double k = lame.m_bulk_mod;
	double kv = k * rest_area;
	double w2 = weight*weight;
	zi_T = (kv*p + w2*zi_T) / (w2 + kv);

	// Apply strain limiting
	bool check_strain = limit[0] > 0.0 || limit[1] < 99.0;
	if (check_strain)
	{
		double l_col0 = zi_T.col(0).norm();
		double l_col1 = zi_T.col(1).norm();
		if (l_col0 < limit[0]) { zi_T.col(0) *= ( limit[0]/l_col0 ); }
		if (l_col1 < limit[0]) { zi_T.col(1) *= ( limit[0]/l_col1 ); }
		if (l_col0 > limit[1]) { zi_T.col(0) *= ( limit[1]/l_col0 ); }
		if (l_col1 > limit[1]) { zi_T.col(1) *= ( limit[1]/l_col1 ); }
	}

	ui += (Dix - zi_T.transpose());
	u->block<2,3>(index,0) = ui;
	z->block<2,3>(index,0) = zi_T.transpose();
}

int EnergyTerm::init_tet(
	const Options *options,
	int index,
	const Eigen::Matrix<int,1,4> &prim,
	const Eigen::MatrixXd *x,
	double &volume,
	double &weight,
	std::vector< Eigen::Triplet<double> > &triplets )
{
	Lame lame;
	lame.set_from_youngs_poisson(options->youngs,options->poisson);

	Matrix<double,3,3> edges;
	edges.col(0) = x->row(prim[1]) - x->row(prim[0]);
	edges.col(1) = x->row(prim[2]) - x->row(prim[0]);
	edges.col(2) = x->row(prim[3]) - x->row(prim[0]);
	Matrix<double,3,3> edges_inv = edges.inverse();
	volume = edges.determinant() / 6.0f;
	if (volume < 0)
	{
		printf("**admmpd::EnergyTerm Error: Inverted initial tet: %f\n",volume);
		return 0;
	}
	double k = lame.m_bulk_mod;
	weight = std::sqrt(k*volume);
	Matrix<double,4,3> S = Matrix<double,4,3>::Zero();
	S(0,0) = -1; S(0,1) = -1; S(0,2) = -1;
	S(1,0) =  1; S(2,1) =  1; S(3,2) =  1;
	Eigen::Matrix<double,3,4> D = (S * edges_inv).transpose();
	int rows[3] = { index+0, index+1, index+2 };
	int cols[4] = { prim[0], prim[1], prim[2], prim[3] };
	for( int r=0; r<3; ++r )
	{
		for( int c=0; c<4; ++c )
		{
			triplets.emplace_back(rows[r], cols[c], D(r,c));
		}
	}
	return 3;
}

int EnergyTerm::init_triangle(
	const Options *options,
	int index,
	const Eigen::RowVector3i &prim,
	const Eigen::MatrixXd *x,
	double &area,
	double &weight,
	std::vector< Eigen::Triplet<double> > &triplets)
{
	Lame lame;
	lame.set_from_youngs_poisson(options->youngs,options->poisson);

	Matrix<double,3,2> edges;
	edges.col(0) = x->row(prim[1]) - x->row(prim[0]);
	edges.col(1) = x->row(prim[2]) - x->row(prim[0]);
	Matrix<double,3,2> basis;
	basis.col(0) = edges.col(0).normalized();
	double dot = edges.col(1).dot(basis.col(0));
	basis.col(1) = (edges.col(1)-dot*basis.col(0)).normalized();
	Matrix<double,2,2> rest_pose = (basis.transpose() * edges).inverse();
	area = (basis.transpose() * edges).determinant() / 2.0f;
	if (area < 0)
	{
		printf("**admmpd::TriEnergyTerm Error: Inverted initial tri: %f\n",area);
		return 0;
	}
	double k = lame.m_bulk_mod;
	weight = std::sqrt(k*area);
	Matrix<double,3,2> S = Matrix<double,3,2>::Zero();
	S(0,0) = -1; S(0,1) = -1;
	S(1,0) =  1; S(2,1) =  1;
	Eigen::Matrix<double,2,3> D = (S * rest_pose).transpose();
	int rows[2] = { index+0, index+1 };
	int cols[3] = { prim[0], prim[1], prim[2] };
	for (int r=0; r<2; ++r)
	{
		for (int c=0; c<3; ++c)
		{
			triplets.emplace_back(rows[r], cols[c], D(r,c));
		}
	}
	return 2;
}

void EnergyTerm::solve_prox(
	const Options *options,
	int index,
	const Lame &lame,
	const Eigen::Vector3d &s0,
	Eigen::Vector3d &s)
{
	(void)(index);

	// Always start prox solve at positive singular vals
	if( s[2] < 0.0 ){ s[2] *= -1.0; }
//	s = Vector3d::Ones();

	Vector3d g; // gradient
	Vector3d p; // descent
	Vector3d s_prev;
	Matrix3d H = Matrix3d::Identity();
	double energy_k, energy_k1;
	const bool add_admm_pen = true;
	const double eps = 1e-6;
	const int max_ls_iter = 20;

	int iter = 0;
	for(; iter<10; ++iter)
	{
		g.setZero();
		energy_k = energy_density(options,lame,add_admm_pen,s0,s,&g); // e and g

		// Converged because low gradient
		if (g.norm() < eps)
			break;

		hessian_spd(options,lame,add_admm_pen,s,H);
		p = H.ldlt().solve(-g); // Newton step direction

		s_prev = s;
		s = s_prev + p;
		energy_k1 = energy_density(options,lame,add_admm_pen,s0,s);

		// Backtracking line search
		double alpha = 1;
		int ls_iter = 0;
		while(energy_k1>energy_k && ls_iter<max_ls_iter)
		{
			alpha *= 0.5;
			s = s_prev + alpha*p;
			energy_k1 = energy_density(options,lame,add_admm_pen,s0,s);
			ls_iter++;
		}

		// Sometimes flattened tets will have a hard time
		// uninverting, in which case they get linesearch
		// blocked. There are ways around this, but for now
		// simply quitting is fine. The global step will smooth
		// things over and get us in a better state next time.
		if (ls_iter>=max_ls_iter)
		{
			s = s_prev;
			break;
		}

		// Stopped making progress
		if ((s-s_prev).norm() < eps)
			break;

	} // end Newton iterations

	if (std::isnan(s[0]) || std::isnan(s[1]) || std::isnan(s[2]))
		throw std::runtime_error("*EnergyTerm::solve_prox: Got nans");

} // end solve prox

double EnergyTerm::energy_density(
	const Options *options,
	const Lame &lame,
	bool add_admm_penalty,
	const Eigen::Vector3d &s0,
	const Eigen::Vector3d &s,
	Eigen::Vector3d *g)
{
	double e = 0;

	// Compute energy and gradient
	// https://github.com/mattoverby/admm-elastic/blob/master/src/TetEnergyTerm.cpp
	// I suppose I should add ARAP for completeness even though it's not used
	switch (options->elastic_material)
	{
		default: {
			if (g != nullptr) g->setZero();
		} break;

		case ELASTIC_NH: {
			if (s.minCoeff() <= 0)
			{ // barrier energy to prevent inversions
				if (g != nullptr) g->setZero();
				return std::numeric_limits<float>::max();
			}
			double J = s.prod();
			double I_1 = s.dot(s);
			double log_I3 = std::log(J*J);
			double t1 = 0.5 * lame.m_mu * (I_1-log_I3-3.0);
			double t2 = 0.125 * lame.m_lambda * log_I3 * log_I3;
			e = t1 + t2;
			if (g != nullptr)
			{
				Vector3d s_inv = s.cwiseInverse();
				*g = lame.m_mu*(s-s_inv) + lame.m_lambda*std::log(J)*s_inv;
			}
		} break;
	}

	if (add_admm_penalty)
	{
		const double &k = lame.m_bulk_mod;
		Vector3d s_min_s0 = s-s0;
		e += (k*0.5) * s_min_s0.squaredNorm();
		if (g != nullptr) *g = *g + k*s_min_s0;
	}

	return e;

} // end energy

void EnergyTerm::hessian_spd(
	const Options *options,
	const Lame &lame,
	bool add_admm_penalty,
	const Eigen::Vector3d &s,
	Eigen::Matrix3d &H)
{
	static const Matrix3d I = Matrix3d::Identity();

	// Compute specific Hessian
	switch (options->elastic_material)
	{
		default:
		{
			H.setIdentity(); // We don't use ARAP hessian
		} break;

		case ELASTIC_NH:
		{
			double J = s.prod();
			Vector3d s_inv = s.cwiseInverse();
			Matrix3d P = Matrix3d::Zero();
			P(0,0) = 1.0 / (s[0]*s[0]);
			P(1,1) = 1.0 / (s[1]*s[1]);
			P(2,2) = 1.0 / (s[2]*s[2]);
			H =	lame.m_mu*(I - 2.0*P) +
				lame.m_lambda * std::log(J) * P +
				lame.m_lambda * s_inv * s_inv.transpose();
		} break;
	}

	// ADMM penalty
	if(add_admm_penalty)
	{
		const double &k = lame.m_bulk_mod;
		for (int i=0; i<3; ++i)
			H(i,i) += k;
	}

	// Projects a matrix to nearest SPD
	// https://github.com/penn-graphics-research/DOT/blob/master/src/Utils/IglUtils.hpp
	auto project_spd = [](Matrix3d &H_)
	{
		SelfAdjointEigenSolver<Matrix3d> eigenSolver(H_);
		if (eigenSolver.eigenvalues()[0] >= 0.0)
			return;
		Eigen::DiagonalMatrix<double,3> D(eigenSolver.eigenvalues());
		for (int i = 0; i<3; ++i)
		{
			if (D.diagonal()[i] < 0)
				D.diagonal()[i] = 0;
			else
				break;
		}
		H_ = eigenSolver.eigenvectors()*D*eigenSolver.eigenvectors().transpose();
	};

	project_spd(H);

} // end hessian

} // end namespace mcl
