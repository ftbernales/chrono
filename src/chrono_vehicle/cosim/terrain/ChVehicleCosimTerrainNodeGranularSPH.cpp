// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2020 projectchrono.org
// All right reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors: Wei Hu, Radu Serban
// =============================================================================
//
// Definition of the SPH granular TERRAIN NODE (using Chrono::FSI).
//
// The global reference frame has Z up, X towards the front of the vehicle, and
// Y pointing to the left.
//
// =============================================================================

#include <algorithm>
#include <cmath>
#include <set>

#include "chrono/utils/ChUtilsCreators.h"
#include "chrono/utils/ChUtilsGenerators.h"
#include "chrono/utils/ChUtilsInputOutput.h"

#include "chrono/assets/ChBoxShape.h"
#include "chrono/assets/ChTriangleMeshShape.h"

#include "chrono_fsi/utils/ChUtilsPrintSph.cuh"

#include "chrono_vehicle/cosim/terrain/ChVehicleCosimTerrainNodeGranularSPH.h"

using std::cout;
using std::endl;

using namespace chrono::fsi;
using namespace rapidjson;

namespace chrono {
namespace vehicle {

// Obstacle bodies have identifier larger than this value
static const int body_id_obstacles = 100000;

// -----------------------------------------------------------------------------
// Construction of the terrain node:
// - create the Chrono system and set solver parameters
// - create the Chrono FSI system
// -----------------------------------------------------------------------------
ChVehicleCosimTerrainNodeGranularSPH::ChVehicleCosimTerrainNodeGranularSPH(double length, double width)
    : ChVehicleCosimTerrainNodeChrono(Type::GRANULAR_SPH, length, width, ChContactMethod::SMC), m_depth(0) {
    // Default granular material properties
    m_radius_g = 0.01;
    m_rho_g = 2000;

    // Create systems
    m_system = new ChSystemSMC;
    m_systemFSI = new ChSystemFsi(*m_system);

    // Solver settings independent of method type
    m_system->Set_G_acc(ChVector<>(0, 0, m_gacc));

    // Set number of threads
    m_system->SetNumThreads(1);

    // Create OpenGL visualization system
#ifdef CHRONO_OPENGL
    m_vsys = new opengl::ChVisualSystemOpenGL;
#endif
}

ChVehicleCosimTerrainNodeGranularSPH::ChVehicleCosimTerrainNodeGranularSPH(const std::string& specfile)
    : ChVehicleCosimTerrainNodeChrono(Type::GRANULAR_SPH, 0, 0, ChContactMethod::SMC) {
    // Create systems
    m_system = new ChSystemSMC;
    m_systemFSI = new ChSystemFsi(*m_system);

    // Solver settings independent of method type
    m_system->Set_G_acc(ChVector<>(0, 0, m_gacc));

    // Set number of threads
    m_system->SetNumThreads(1);

    // Read SPH granular terrain parameters from provided specfile
    SetFromSpecfile(specfile);

    // Create OpenGL visualization system
#ifdef CHRONO_OPENGL
    m_vsys = new opengl::ChVisualSystemOpenGL;
#endif
}

ChVehicleCosimTerrainNodeGranularSPH::~ChVehicleCosimTerrainNodeGranularSPH() {
    delete m_systemFSI;
    delete m_system;
#ifdef CHRONO_OPENGL
    delete m_vsys;
#endif
}

// -----------------------------------------------------------------------------

//// TODO: error checking
void ChVehicleCosimTerrainNodeGranularSPH::SetFromSpecfile(const std::string& specfile) {
    Document d;
    ReadSpecfile(specfile, d);

    double length = d["Patch dimensions"]["Length"].GetDouble();
    double width = d["Patch dimensions"]["Width"].GetDouble();
    m_hdimX = length / 2;
    m_hdimY = width / 2;

    m_radius_g = d["Granular material"]["Radius"].GetDouble();
    m_rho_g = d["Granular material"]["Density"].GetDouble();
    m_depth = d["Granular material"]["Depth"].GetDouble();
    m_init_height = m_depth;

    // Get the pointer to the system parameter and use a JSON file to fill it out with the user parameters
    m_systemFSI->ReadParametersFromFile(specfile);
}

void ChVehicleCosimTerrainNodeGranularSPH::SetGranularMaterial(double radius, double density) {
    m_radius_g = radius;
    m_rho_g = density;
}

void ChVehicleCosimTerrainNodeGranularSPH::SetPropertiesSPH(const std::string& filename, double depth) {
    m_depth = depth;
    m_init_height = m_depth;

    // Get the pointer to the system parameter and use a JSON file to fill it out with the user parameters
    m_systemFSI->ReadParametersFromFile(filename);
}

// -----------------------------------------------------------------------------
// Complete construction of the mechanical system.
// This function is invoked automatically from OnInitialize.
// - adjust system settings
// - create the container body
// - add fluid particles
// - create obstacle bodies (if any)
// -----------------------------------------------------------------------------
void ChVehicleCosimTerrainNodeGranularSPH::Construct() {
    if (m_verbose)
        cout << "[Terrain node] GRANULAR_SPH " << endl;

    // Reload simulation parameters to FSI system
    double initSpace0 = 2 * m_radius_g;
    m_systemFSI->SetStepSize(m_step_size, m_step_size);
    m_systemFSI->Set_G_acc(ChVector<>(0, 0, m_gacc));
    m_systemFSI->SetDensity(m_rho_g);
    m_systemFSI->SetInitialSpacing(initSpace0);
    m_systemFSI->SetKernelLength(initSpace0);

    // Set up the periodic boundary condition (if not, set relative larger values)
    ChVector<> cMin(-2 * m_hdimX, -2 * m_hdimY, -10 * m_depth - 10 * initSpace0);
    ChVector<> cMax(+2 * m_hdimX, +2 * m_hdimY, +20 * m_depth + 10 * initSpace0);
    m_systemFSI->SetBoundaries(cMin, cMax);

    // Set the time integration type and the linear solver type (only for ISPH)
    m_systemFSI->SetSPHMethod(FluidDynamics::WCSPH);

    // Set boundary condition for the fixed wall
    m_systemFSI->SetWallBC(BceVersion::ORIGINAL);

    // Create fluid region and discretize with SPH particles
    ChVector<> boxCenter(0.0, 0.0, m_depth / 2);
    ChVector<> boxHalfDim(m_hdimX, m_hdimY, m_depth / 2);

    // Use a chrono sampler to create a bucket of points
    utils::GridSampler<> sampler(initSpace0);
    utils::Generator::PointVector points = sampler.SampleBox(boxCenter, boxHalfDim);

    // Add fluid particles from the sampler points to the FSI system
    int numPart = (int)points.size();
    for (int i = 0; i < numPart; i++) {
        // Calculate the pressure of a steady state (p = rho*g*h)
        fsi::Real pre_ini = m_systemFSI->GetDensity() * abs(m_gacc) * (-points[i].z() + m_depth);
        fsi::Real rho_ini =
            m_systemFSI->GetDensity() + pre_ini / (m_systemFSI->GetSoundSpeed() * m_systemFSI->GetSoundSpeed());
        m_systemFSI->AddSPHParticle(points[i], rho_ini, 0.0, m_systemFSI->GetViscosity(), ChVector<>(1e-10),
                                    ChVector<>(-pre_ini), ChVector<>(1e-10));
    }

    // Create a body for the fluid container body
    auto container = std::shared_ptr<ChBody>(m_system->NewBody());
    m_system->AddBody(container);
    container->SetIdentifier(-1);
    container->SetMass(1);
    container->SetBodyFixed(true);
    container->SetCollide(false);

    // Create the geometry of the boundaries
    m_systemFSI->AddContainerBCE(container, ChFrame<>(), ChVector<>(2 * m_hdimX, 2 * m_hdimY, 1.25 * m_depth),
                                 ChVector<int>(2, 2, -1));

    // Add all rigid obstacles
    int id = body_id_obstacles;
    for (auto& b : m_obstacles) {
        auto mat = b.m_contact_mat.CreateMaterial(m_system->GetContactMethod());
        auto trimesh = chrono_types::make_shared<geometry::ChTriangleMeshConnected>();
        trimesh->LoadWavefrontMesh(GetChronoDataFile(b.m_mesh_filename), true, true);
        double mass;
        ChVector<> baricenter;
        ChMatrix33<> inertia;
        trimesh->ComputeMassProperties(true, mass, baricenter, inertia);

        auto body = std::shared_ptr<ChBody>(m_system->NewBody());
        body->SetNameString("obstacle");
        body->SetIdentifier(id++);
        body->SetPos(b.m_init_pos);
        body->SetRot(b.m_init_rot);
        body->SetMass(mass * b.m_density);
        body->SetInertia(inertia * b.m_density);
        body->SetBodyFixed(false);
        body->SetCollide(true);

        body->GetCollisionModel()->ClearModel();
        body->GetCollisionModel()->AddTriangleMesh(mat, trimesh, false, false, ChVector<>(0), ChMatrix33<>(1),
                                                   m_radius_g);
        body->GetCollisionModel()->SetFamily(2);
        body->GetCollisionModel()->BuildModel();

        auto trimesh_shape = chrono_types::make_shared<ChTriangleMeshShape>();
        trimesh_shape->SetMesh(trimesh);
        trimesh_shape->SetName(filesystem::path(b.m_mesh_filename).stem());
        body->AddVisualShape(trimesh_shape, ChFrame<>());

        m_system->AddBody(body);

        // Add this body to the FSI system
        m_systemFSI->AddFsiBody(body);

        // Create BCE markers associated with trimesh
        std::vector<ChVector<>> point_cloud;
        m_systemFSI->CreateMeshPoints(*trimesh, (double)initSpace0, point_cloud);
        m_systemFSI->AddPointsBCE(body, point_cloud, ChFrame<>(), true);
    }

#ifdef CHRONO_OPENGL
    // Add visualization asset for the container
    auto box = chrono_types::make_shared<ChBoxShape>();
    box->GetBoxGeometry().Size = ChVector<>(m_hdimX, m_hdimY, m_depth / 2);
    container->AddVisualShape(box, ChFrame<>(ChVector<>(0, 0, m_depth / 2)));

    // Create the visualization window
    if (m_render) {
        m_vsys->AttachSystem(m_system);
        m_vsys->SetWindowTitle("Terrain Node (GranularSPH)");
        m_vsys->SetWindowSize(1280, 720);
        m_vsys->SetRenderMode(opengl::WIREFRAME);
        m_vsys->Initialize();
        m_vsys->SetCameraPosition(ChVector<>(0, -6, 0), ChVector<>(0, 0, 0));
        m_vsys->SetCameraProperties(0.05f);
        m_vsys->SetCameraVertical(CameraVerticalDir::Z);
    }
#endif

    // Write file with terrain node settings
    std::ofstream outf;
    outf.open(m_node_out_dir + "/settings.info", std::ios::out);
    outf << "System settings" << endl;
    outf << "   Integration step size = " << m_step_size << endl;
    outf << "Patch dimensions" << endl;
    outf << "   X = " << 2 * m_hdimX << "  Y = " << 2 * m_hdimY << endl;
    outf << "   depth = " << m_depth << endl;
}

// -----------------------------------------------------------------------------

void ChVehicleCosimTerrainNodeGranularSPH::CreateRigidProxy(unsigned int i) {
    // Number of rigid obstacles
    auto num_obstacles = m_obstacles.size();

    // Create wheel proxy body
    auto body = std::shared_ptr<ChBody>(m_system->NewBody());
    body->SetIdentifier(0);
    body->SetMass(m_load_mass[i]);
    body->SetBodyFixed(true);  // proxy body always fixed
    body->SetCollide(false);

    // Get shape associated with the given object
    int i_shape = m_obj_map[i];

    // Create visualization asset (use collision shapes)
    m_geometry[i_shape].CreateVisualizationAssets(body, VisualizationType::PRIMITIVES, true);

    // Create collision shapes (only if obstacles are present)
    if (num_obstacles > 0) {
        for (auto& mesh : m_geometry[i_shape].m_coll_meshes)
            mesh.m_radius = m_radius_g;
        m_geometry[i_shape].CreateCollisionShapes(body, 1, m_method);
        body->GetCollisionModel()->SetFamily(1);
        body->GetCollisionModel()->SetFamilyMaskNoCollisionWithFamily(1);
    }

    m_system->AddBody(body);
    m_proxies[i].push_back(ProxyBody(body, 0));

    // Add this body to the FSI system
    m_systemFSI->AddFsiBody(body);

    // Create BCE markers associated with collision shapes
    for (const auto& box : m_geometry[i_shape].m_coll_boxes) {
        m_systemFSI->AddBoxBCE(body, ChFrame<>(box.m_pos, box.m_rot), box.m_dims, true);
    }
    for (const auto& sphere : m_geometry[i_shape].m_coll_spheres) {
        m_systemFSI->AddSphereBCE(body, ChFrame<>(sphere.m_pos, QUNIT), sphere.m_radius, true);
    }
    for (const auto& cyl : m_geometry[i_shape].m_coll_cylinders) {
        m_systemFSI->AddCylinderBCE(body, ChFrame<>(cyl.m_pos, cyl.m_rot), cyl.m_radius, cyl.m_length, true);
    }
    for (const auto& mesh : m_geometry[i_shape].m_coll_meshes) {
        std::vector<ChVector<>> point_cloud;
        m_systemFSI->CreateMeshPoints(*mesh.m_trimesh, (double)m_systemFSI->GetInitialSpacing(), point_cloud);
        m_systemFSI->AddPointsBCE(body, point_cloud, ChFrame<>(VNULL, QUNIT), true);
    }
}

// Once all proxy bodies are created, complete construction of the underlying FSI system.
void ChVehicleCosimTerrainNodeGranularSPH::OnInitialize(unsigned int num_objects) {
    ChVehicleCosimTerrainNodeChrono::OnInitialize(num_objects);
    m_systemFSI->Initialize();
}

// Set state of proxy rigid body.
void ChVehicleCosimTerrainNodeGranularSPH::UpdateRigidProxy(unsigned int i, BodyState& rigid_state) {
    auto& proxies = m_proxies[i];  // proxies for the i-th rigid

    proxies[0].m_body->SetPos(rigid_state.pos);
    proxies[0].m_body->SetPos_dt(rigid_state.lin_vel);
    proxies[0].m_body->SetRot(rigid_state.rot);
    proxies[0].m_body->SetWvel_par(rigid_state.ang_vel);
    proxies[0].m_body->SetWacc_par(ChVector<>(0.0, 0.0, 0.0));
}

// Collect resultant contact force and torque on rigid proxy body.
void ChVehicleCosimTerrainNodeGranularSPH::GetForceRigidProxy(unsigned int i, TerrainForce& rigid_contact) {
    const auto& proxies = m_proxies[i];  // proxies for the i-th rigid

    rigid_contact.point = ChVector<>(0, 0, 0);
    rigid_contact.force = proxies[0].m_body->Get_accumulated_force();
    rigid_contact.moment = proxies[0].m_body->Get_accumulated_torque();
}

// -----------------------------------------------------------------------------

void ChVehicleCosimTerrainNodeGranularSPH::CreateMeshProxy(unsigned int i) {


}

void ChVehicleCosimTerrainNodeGranularSPH::UpdateMeshProxy(unsigned int i, MeshState& mesh_state) {}

void ChVehicleCosimTerrainNodeGranularSPH::GetForceMeshProxy(unsigned int i, MeshContact& mesh_contact) {}

// -----------------------------------------------------------------------------

void ChVehicleCosimTerrainNodeGranularSPH::OnAdvance(double step_size) {
    double t = 0;
    while (t < step_size) {
        double h = std::min<>(m_step_size, step_size - t);
        m_systemFSI->DoStepDynamics_FSI();
        t += h;
    }
}

void ChVehicleCosimTerrainNodeGranularSPH::Render(double time) {
#ifdef CHRONO_OPENGL
    if (m_vsys->Run()) {
        const auto& proxies = m_proxies[0];  // proxies for first object
        ChVector<> cam_point = proxies[0].m_body->GetPos();
        ChVector<> cam_loc = cam_point + ChVector<>(0, -3, 0.6);
        m_vsys->SetCameraPosition(cam_loc, cam_point);
        m_vsys->Render();
    } else {
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
#endif
}

// -----------------------------------------------------------------------------

void ChVehicleCosimTerrainNodeGranularSPH::OnOutputData(int frame) {
    // Save SPH and BCE particles' information into CSV files
    m_systemFSI->PrintParticleToFile(m_node_out_dir + "/simulation");
}

void ChVehicleCosimTerrainNodeGranularSPH::OutputVisualizationData(int frame) {
    auto filename = OutputFilename(m_node_out_dir + "/visualization", "vis", "chpf", frame, 5);
    m_systemFSI->SetParticleOutputMode(ChSystemFsi::OutpuMode::CHPF);
    m_systemFSI->WriteParticleFile(filename);
    if (m_obstacles.size() > 0) {
        filename = OutputFilename(m_node_out_dir + "/visualization", "vis", "dat", frame, 5);
        // Include only obstacle bodies
        utils::WriteVisualizationAssets(
            m_system, filename, [](const ChBody& b) -> bool { return b.GetIdentifier() >= body_id_obstacles; }, true);
    }
}

void ChVehicleCosimTerrainNodeGranularSPH::PrintMeshProxiesUpdateData(unsigned int i, const MeshState& mesh_state) {}

}  // end namespace vehicle
}  // end namespace chrono
