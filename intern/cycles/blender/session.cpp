/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <cstdlib>

#include "device/device.h"

#include "scene/background.h"
#include "scene/bake.h"
#include "scene/camera.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "scene/light.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/stats.h"

#include "session/buffers.h"
#include "session/session.h"

#include "util/hash.h"
#include "util/log.h"
#include "util/murmurhash.h"
#include "util/path.h"
#include "util/progress.h"
#include "util/time.h"

#include "blender/display_driver.h"
#include "blender/output_driver.h"
#include "blender/session.h"
#include "blender/sync.h"
#include "blender/util.h"

// BRAAS-HPC
#include "braas_hpc_display_driver.h"
#include "renderengine_tcp.h"
#include "renderengine_data.h"
#include "app/cycles_xml_bin.h"

#ifdef DEBUG_TIME
#	define DEBUG_START_TIME(name) double t1_##name = omp_get_wtime();
#	define DEBUG_END_TIME(name) double t2_##name = omp_get_wtime(); printf("Elapsed time: %f, FPS: %f, %s\n", t2_##name - t1_##name, 1.0 / (t2_##name - t1_##name), #name);
#else
#	define DEBUG_START_TIME(name)
#	define DEBUG_END_TIME(name)
#endif

CCL_NAMESPACE_BEGIN

// BRAAS-HPC
struct BraaSHPCOptions {
    int id = 0;

    //Session* session = nullptr;
    //Scene* scene = nullptr;
    std::string filepath;
    int width, height;
    SceneParams scene_params;
    SessionParams session_params;
    bool quiet;
    bool show_help, interactive, pause;
    //std::string output_filepath;
    std::string output_pass;
    int session_samples = 0;

    //FrameOutputDriver* output_driver = nullptr;
    BRaaSHPCDisplayDriver* display_driver = nullptr;
};

renderengine_data g_renderengine_data_rcv;
//std::vector<renderengine_data> g_renderengine_datas;
renderengine_data g_renderengine_data;
double fps_previous_time = 0;
int fps_frame_count = 0;

struct CyclesphiDataRenderAux {
    std::vector<char> data;
};

///////

DeviceTypeMask BlenderSession::device_override = DEVICE_MASK_ALL;
bool BlenderSession::headless = false;
bool BlenderSession::print_render_stats = false;

BlenderSession::BlenderSession(BL::RenderEngine &b_engine,
                               BL::Preferences &b_userpref,
                               BL::BlendData &b_data,
                               bool preview_osl)
    : session(nullptr),
      scene(nullptr),
      sync(nullptr),
      b_engine(b_engine),
      b_userpref(b_userpref),
      b_data(b_data),
      b_render(b_engine.render()),
      b_depsgraph(PointerRNA_NULL),
      b_scene(PointerRNA_NULL),
      b_v3d(PointerRNA_NULL),
      b_rv3d(PointerRNA_NULL),
      width(0),
      height(0),
      preview_osl(preview_osl),
      python_thread_state(nullptr),
      use_developer_ui(b_userpref.experimental().use_cycles_debug() &&
                       b_userpref.view().show_developer_ui())
{
  /* offline render */
  background = true;
  last_redraw_time = 0.0;
  start_resize_time = 0.0;
  last_status_time = 0.0;
  braas_hpc_options = nullptr;

  //BRAAS-HPC
  const char* env_p = std::getenv("CYCLES_BRAAS_HPC_INTERACTIVE_MODE");
  if (env_p != nullptr && atoi(env_p) != 0) {
      braas_hpc_options = new BraaSHPCOptions();
      // background = false;
  }
}

BlenderSession::BlenderSession(BL::RenderEngine &b_engine,
                               BL::Preferences &b_userpref,
                               BL::BlendData &b_data,
                               BL::SpaceView3D &b_v3d,
                               BL::RegionView3D &b_rv3d,
                               const int width,
                               const int height)
    : session(nullptr),
      scene(nullptr),
      sync(nullptr),
      b_engine(b_engine),
      b_userpref(b_userpref),
      b_data(b_data),
      b_render(b_engine.render()),
      b_depsgraph(PointerRNA_NULL),
      b_scene(PointerRNA_NULL),
      b_v3d(b_v3d),
      b_rv3d(b_rv3d),
      width(width),
      height(height),
      preview_osl(false),
      python_thread_state(nullptr),
      use_developer_ui(b_userpref.experimental().use_cycles_debug() &&
                       b_userpref.view().show_developer_ui())
{
  /* 3d view render */
  background = false;
  last_redraw_time = 0.0;
  start_resize_time = 0.0;
  last_status_time = 0.0;
  braas_hpc_options = nullptr;

  //BRAAS-HPC
  const char* env_p = std::getenv("CYCLES_BRAAS_HPC_INTERACTIVE_MODE");
  if (env_p != nullptr && atoi(env_p) != 0) {
      braas_hpc_options = new BraaSHPCOptions();
      // background = false;
  }
}

BlenderSession::~BlenderSession()
{
  free_session();

  if (braas_hpc_options) {
	  delete (BraaSHPCOptions*)braas_hpc_options;
	  braas_hpc_options = nullptr;
  }
}

////////////////////////////////////////BRAAS-HPC

SessionParams BlenderSession::get_session_params(BL::RenderEngine& b_engine,
    BL::Preferences& b_preferences,
    BL::Scene& b_scene,
    bool background) {

    if (braas_hpc_options) {
		BraaSHPCOptions* options = (BraaSHPCOptions*)braas_hpc_options;
		return options->session_params;
    }

    return BlenderSync::get_session_params(
        b_engine, b_userpref, b_scene, background);
}

SceneParams BlenderSession::get_scene_params(BL::Scene& b_scene,
    const bool background,
    const bool use_developer_ui) {

    if (braas_hpc_options) {
		BraaSHPCOptions* options = (BraaSHPCOptions*)braas_hpc_options;
		return options->scene_params;
	}

    return BlenderSync::get_scene_params(
        b_scene, background, use_developer_ui);
}

BufferParams& BlenderSession::braas_hpc_session_buffer_params()
{
    BraaSHPCOptions* options = (BraaSHPCOptions*)braas_hpc_options;
    static BufferParams buffer_params;
    buffer_params.width = options->width;
    buffer_params.height = options->height;
    buffer_params.full_width = options->width;
    buffer_params.full_height = options->height;

    return buffer_params;
}

void BlenderSession::braas_hpc_session_init(SessionParams& session_params, SceneParams& scene_params)
{
    BraaSHPCOptions* options = (BraaSHPCOptions*)braas_hpc_options;

    options->session_samples = 0;
    session_params.background = background;
    session_params.headless = false;
    session_params.use_auto_tile = false;
    session_params.tile_size = 16;
    session_params.use_resolution_divider = false;
    session_params.samples = 1;

    //session_params.threads = 1;

    options->output_pass = "combined";

    //session = new Session(options->session_params, options->scene_params);
    session = make_unique<Session>(session_params, scene_params);

    auto display_driver = make_unique<BRaaSHPCDisplayDriver>();
    options->display_driver = display_driver.get();
    session->set_display_driver(std::move(display_driver));

    //if (options->session_params.background && !options->quiet) {
    //    session->progress.set_update_callback([&options]() {session_print_status(options); });
    //}

    /* load scene */
    //scene_init(options);

    /* add pass for output. */
    //Pass* pass = scene->create_node<Pass>();
    //pass->set_name(ustring(options->output_pass.c_str()));
    //pass->set_type(PASS_COMBINED);

    options->session_params = session_params;
    options->scene_params = scene_params;
}

void BlenderSession::braas_hpc_render_frame()
{
    BraaSHPCOptions* options = (BraaSHPCOptions*)braas_hpc_options;

    if (options->display_driver)
        options->display_driver->renderBegin();

    if (options->session_samples == 0) { // reset
        session->reset(options->session_params, braas_hpc_session_buffer_params());
    }

    //if(options->output_driver)
    //	options->output_driver->renderBegin();

    session->set_samples(++options->session_samples);
    session->start();

    //session->wait();
    //session->draw();

    //if (options->output_driver)
    //	options->output_driver->wait();

    //if (options->display_driver)
    //    options->display_driver->wait();

    //session->start();
    session->wait();
}

int BlenderSession::braas_hpc_cyclesphi(void* _blenderClientTcp)
{
	TcpConnection* blenderClientTcp = (TcpConnection*)_blenderClientTcp;
    ////////////////////////////////////////////////////
    double render_time = 0;
    double render_time_accu = 0;
    int spp_one_step = 0;

    std::vector<char> pixels_buf_empty;
    CyclesphiDataRenderAux data_render_aux_rcv;
    //std::vector<CyclesphiDataRenderAux> data_render_aux;
    CyclesphiDataRenderAux data_render_aux;

    /////////
    //int renderengine_data_count = 1;
    //g_renderengine_datas.resize(renderengine_data_count);

    BraaSHPCOptions* main_options = (BraaSHPCOptions*)braas_hpc_options; //&options[0];
    renderengine_data* main_renderengine_data = &g_renderengine_data;

    //data_render_aux.resize(renderengine_data_count);
    CyclesphiDataRenderAux* main_data_render_aux = &data_render_aux;
    /////////

    BRaaSHPCDataState cyclesphiDataState;
    memset(&cyclesphiDataState, 0, sizeof(cyclesphiDataState));

    ///////////////////
    BoundBox bbox_scene = BoundBox::empty;
    bool bbox_computed = false;
    ///////////////////
    bool render_running = true;

    //session_print("Start rendering...\n");

    while (render_running) {
        DEBUG_START_TIME(overall);

        DEBUG_START_TIME(receive);

        blenderClientTcp->recv_data_data((char*)&g_renderengine_data_rcv, sizeof(renderengine_data));
        if (blenderClientTcp->is_error()) {
            break;
        }

        if (g_renderengine_data_rcv.reset) {
            break;
        }

        if (g_renderengine_data_rcv.width == 0 || g_renderengine_data_rcv.height == 0) {
            printf("width or height is 0!!!!\n");
            fflush(0);
            //exit(-1);
            break;
        }

        blenderClientTcp->set_frame(g_renderengine_data_rcv.frame);

        // check animation
        //if (options->size() > 1 && blenderClientTcp->get_frame() >= 0 && blenderClientTcp->get_frame() < options->size()) {
        //    main_options = &options[blenderClientTcp->get_frame()];
        //    main_renderengine_data = &g_renderengine_datas[blenderClientTcp->get_frame()];
        //    main_data_render_aux = &data_render_aux[blenderClientTcp->get_frame()];
        //}

        g_renderengine_data_rcv.frame = main_renderengine_data->frame;

        int cyclesphiDataRenderSize = 0;
        blenderClientTcp->recv_data_data((char*)&cyclesphiDataRenderSize, sizeof(int));

        if (data_render_aux_rcv.data.size() != cyclesphiDataRenderSize) {
            data_render_aux_rcv.data.resize(cyclesphiDataRenderSize);
        }

        if (data_render_aux_rcv.data.size() > 0) {
            blenderClientTcp->recv_data_data((char*)data_render_aux_rcv.data.data(), data_render_aux_rcv.data.size());
            data_render_aux_rcv.data.push_back('\0');
        }

        if (blenderClientTcp->is_error()) {
            //throw std::runtime_error("TCP Error!");
            break;
        }

        if (pixels_buf_empty.size() != sizeof(half4) * g_renderengine_data_rcv.width * g_renderengine_data_rcv.height) {
            pixels_buf_empty.resize(sizeof(half4) * g_renderengine_data_rcv.width * g_renderengine_data_rcv.height);
        }

        DEBUG_END_TIME(receive);

        try {
            // cam_change
            if (/*renderer == NULL || */ memcmp(main_renderengine_data, &g_renderengine_data_rcv, sizeof(renderengine_data))) {
                DEBUG_START_TIME(camera);
                memcpy(main_renderengine_data, &g_renderengine_data_rcv, sizeof(renderengine_data));

                render_time = 0;
                render_time_accu = 0;

                main_options->session_samples = 0;

                if (g_renderengine_data_rcv.reset || main_options->width != g_renderengine_data_rcv.width
                    || main_options->height != g_renderengine_data_rcv.height) {

                    main_options->width = g_renderengine_data_rcv.width;
                    main_options->height = g_renderengine_data_rcv.height;
                }

                float* input = g_renderengine_data_rcv.cam.transform_inverse_view_matrix;

                // Convert to Transform
                Transform tfm = transform_clear_scale(make_transform(
                    input[0], input[1], input[2],   // First row
                    input[3], input[4], input[5],   // Second row
                    input[6], input[7], input[8],   // Third row
                    input[9], input[10], input[11]  // Fourth row (Translation vector)
                ) * transform_scale(1.0f, 1.0f, -1.0f));

                scene->camera->set_matrix(tfm);
                scene->camera->set_full_width(main_options->width);
                scene->camera->set_full_height(main_options->height);

                scene->camera->set_nearclip(g_renderengine_data_rcv.cam.clip_start);
                scene->camera->set_farclip(g_renderengine_data_rcv.cam.clip_end);

                scene->camera->need_flags_update = true;
                scene->camera->need_device_update = true;

                //perspective
                scene->camera->set_fov(g_renderengine_data_rcv.cam.lens);

                if (g_renderengine_data_rcv.cam.view_perspective == 1) { //CAMERA_ORTHOGRAPHIC
                    scene->camera->set_camera_type(CameraType::CAMERA_ORTHOGRAPHIC);
                }
                else {
                    scene->camera->set_camera_type(CameraType::CAMERA_PERSPECTIVE);
                }

                float xratio = (float)main_options->width;
                float yratio = (float)main_options->height;
                bool horizontal_fit = (xratio > yratio);

                float aspectratio;
                float xaspect, yaspect;
                if (horizontal_fit) {
                    aspectratio = xratio / yratio;
                    xaspect = aspectratio;
                    yaspect = 1.0f;
                }
                else {
                    aspectratio = yratio / xratio;
                    xaspect = 1.0f;
                    yaspect = aspectratio;
                }

                if (g_renderengine_data_rcv.cam.view_perspective == 1) { //CAMERA_ORTHOGRAPHIC
                    float ortho_scale = g_renderengine_data_rcv.cam.lens / 2.0f;
                    xaspect = xaspect * ortho_scale;// / (aspectratio * 2.0f);
                    yaspect = yaspect * ortho_scale;// / (aspectratio * 2.0f);
                    //aspectratio = ortho_scale / 2.0f;					
                }

                scene->camera->set_viewplane_left(-xaspect);
                scene->camera->set_viewplane_right(xaspect);
                scene->camera->set_viewplane_bottom(-yaspect);
                scene->camera->set_viewplane_top(yaspect);

                DEBUG_END_TIME(camera);
            }

            // if (renderer == NULL) {
            // 	continue;
            // }

            if (data_render_aux_rcv.data.size() != main_data_render_aux->data.size()) {
                DEBUG_START_TIME(resize_rcv_data);
                main_data_render_aux->data.resize(data_render_aux_rcv.data.size());
                DEBUG_END_TIME(resize_rcv_data);
            }

            if (data_render_aux_rcv.data.size() > 0 && memcmp(main_data_render_aux->data.data(), data_render_aux_rcv.data.data(), data_render_aux_rcv.data.size())) {
                DEBUG_START_TIME(material);
                memcpy(main_data_render_aux->data.data(), data_render_aux_rcv.data.data(), data_render_aux_rcv.data.size());

                main_options->session_samples = 0;
                render_time = 0;
                render_time_accu = 0;

                xml_set_material_to_shader(scene, main_data_render_aux->data.data());
                DEBUG_END_TIME(material);
            }

            /////////////////////////////////////////////////
            DEBUG_START_TIME(render);
            braas_hpc_render_frame();
            DEBUG_END_TIME(render);
            /////////////////////////////////////////////////
            if (main_options->display_driver) {
                if (main_options->display_driver->is_gpujpeg()) {
                    DEBUG_START_TIME(send_gpujpeg_display);
                    if (main_options->display_driver->d_pixels) {
                        blenderClientTcp->send_gpujpeg((char*)main_options->display_driver->d_pixels, pixels_buf_empty.data(), main_options->width, main_options->height, 1);
                    }
                    else {
                        blenderClientTcp->send_gpujpeg((char*)main_options->display_driver->pixels.data(), pixels_buf_empty.data(), main_options->width, main_options->height, 1);
                    }
                    DEBUG_END_TIME(send_gpujpeg_display);
                }
                else {

                    DEBUG_START_TIME(send_gpujpeg_display);
                    blenderClientTcp->send_data_data((char*)main_options->display_driver->pixels.data(), pixels_buf_empty.size());
                    DEBUG_END_TIME(send_gpujpeg_display);
                }
            }

            if (blenderClientTcp->is_error()) {
                throw std::runtime_error("TCP Error!");
            }

            if (!bbox_computed) {
                DEBUG_START_TIME(bbox_computed);
                for (Object* object : scene->objects) {
                    bbox_scene.grow(object->bounds);
                }

                cyclesphiDataState.world_bounds_spatial_lower[0] = bbox_scene.min[0];
                cyclesphiDataState.world_bounds_spatial_lower[1] = bbox_scene.min[1];
                cyclesphiDataState.world_bounds_spatial_lower[2] = bbox_scene.min[2];
                cyclesphiDataState.world_bounds_spatial_upper[0] = bbox_scene.max[0];
                cyclesphiDataState.world_bounds_spatial_upper[1] = bbox_scene.max[1];
                cyclesphiDataState.world_bounds_spatial_upper[2] = bbox_scene.max[2];

                bbox_computed = true;
                DEBUG_END_TIME(bbox_computed);
            }

            DEBUG_START_TIME(send_data_state);
            float duration = 0;
            //if (main_options->output_driver)
            //	duration = main_options->output_driver->duration;
            if (main_options->display_driver)
                duration = main_options->display_driver->duration;

            cyclesphiDataState.fps = (float)main_options->session_params.samples / duration;//fps;
            cyclesphiDataState.samples = main_options->session_samples;//total_samples;
            blenderClientTcp->send_data_data((char*)&cyclesphiDataState, sizeof(cyclesphiDataState));
            DEBUG_END_TIME(send_data_state);

            if (blenderClientTcp->is_error()) {
                throw std::runtime_error("TCP Error!");
            }
        }
        catch (const std::exception& ex)
        {
            std::cerr << ex.what();
            //exit(-1);
            break;
        }

        DEBUG_END_TIME(overall);
    }

    //////////////////////////////////////////////////// 	

    // reset
    memset(&g_renderengine_data_rcv, 0, sizeof(g_renderengine_data_rcv));

    //g_renderengine_datas.clear();

    return 0;
}

int BlenderSession::braas_hpc_run()
{
    //FromCL fromCL;
    //fromCL.parse_args(ac, av);

    //std::vector<BraaSHPCOptions> options(fromCL.anim > 0 ? fromCL.anim : 1);

    TcpConnection blenderClientTcp;
    //blenderClientTcp.init_sockets_data("localhost", fromCL.port);

    //for (int i = 0; i < options->size(); i++) {
    //    auto& op = options[i];
    //    session_init(fromCL, op, i);
    //}

    int server_port = getenv("CYCLES_BRAAS_HPC_SERVER_PORT") ? atoi(getenv("CYCLES_BRAAS_HPC_SERVER_PORT")) : 7000;

    while (true) {

        //if (blenderClientTcp.is_error()) 
        {
            blenderClientTcp.client_close();
            blenderClientTcp.server_close();
            blenderClientTcp.init_sockets_data("localhost", server_port);
        }

        braas_hpc_cyclesphi(&blenderClientTcp);
    }

    //for (auto& op : options) {
    //    session_exit(fromCL, op);
    //}

    return 0;
}

/////////////////////////////////////////////////

void BlenderSession::create_session()
{
  SessionParams session_params = BlenderSync::get_session_params(
      b_engine, b_userpref, b_scene, background);
  SceneParams scene_params = BlenderSync::get_scene_params(
      b_scene, background, use_developer_ui);
  const bool session_pause = BlenderSync::get_session_pause(b_scene, background);

  /* reset status/progress */
  last_status = "";
  last_error = "";
  last_progress = -1.0;
  start_resize_time = 0.0;

  /* create session */
  //BRAAS-HPC
  if (braas_hpc_options) {
	  braas_hpc_session_init(session_params, scene_params);
  }
  else {
      session = make_unique<Session>(session_params, scene_params);
  }
  
  session->progress.set_update_callback([this] { tag_redraw(); });
  session->progress.set_cancel_callback([this] { test_cancel(); });
  session->set_pause(session_pause);

  /* create scene */
  scene = session->scene.get();
  scene->name = b_scene.name();

  /* create sync */
  sync = make_unique<BlenderSync>(
      b_engine, b_data, b_scene, scene, !background, use_developer_ui, session->progress);
  BL::Object b_camera_override(b_engine.camera_override());
  if (b_v3d) {
    sync->sync_view(b_v3d, b_rv3d, width, height);
  }
  else {
    sync->sync_camera(b_render, b_camera_override, width, height, "");
  }

  /* set buffer parameters */
  const BufferParams buffer_params = BlenderSync::get_buffer_params(
      b_v3d, b_rv3d, scene->camera, width, height);
  session->reset(session_params, buffer_params);

  /* Viewport and preview (as in, material preview) does not do tiled rendering, so can inform
   * engine that no tracking of the tiles state is needed.
   * The offline rendering will make a decision when tile is being written. The penalty of asking
   * the engine to keep track of tiles state is minimal, so there is nothing to worry about here
   * about possible single-tiled final render. */
  if (!b_engine.is_preview() && !b_v3d) {
    b_engine.use_highlight_tiles(true);
  }
}

void BlenderSession::reset_session(BL::BlendData &b_data, BL::Depsgraph &b_depsgraph)
{
  /* Update data, scene and depsgraph pointers. These can change after undo. */
  this->b_data = b_data;
  this->b_depsgraph = b_depsgraph;
  this->b_scene = b_depsgraph.scene_eval();
  if (sync) {
    sync->reset(this->b_data, this->b_scene);
  }

  if (preview_osl) {
    PointerRNA cscene = RNA_pointer_get(&b_scene.ptr, "cycles");
    RNA_boolean_set(&cscene, "shading_system", preview_osl);
  }

  if (b_v3d) {
    this->b_render = b_scene.render();
  }
  else {
    this->b_render = b_engine.render();
    width = render_resolution_x(b_render);
    height = render_resolution_y(b_render);
  }

  const bool is_new_session = (session == nullptr);
  if (is_new_session) {
    /* Initialize session and remember it was just created so not to
     * re-create it below.
     */
    create_session();
  }

  if (b_v3d) {
    /* NOTE: We need to create session, but all the code from below
     * will make viewport render to stuck on initialization.
     */
    return;
  }

  const SessionParams session_params = get_session_params(
      b_engine, b_userpref, b_scene, background);
  const SceneParams scene_params = get_scene_params(
      b_scene, background, use_developer_ui);

  if (scene->params.modified(scene_params) || session->params.modified(session_params) ||
      !this->b_render.use_persistent_data())
  {
    /* if scene or session parameters changed, it's easier to simply re-create
     * them rather than trying to distinguish which settings need to be updated
     */
    if (!is_new_session) {
      free_session();
      create_session();
    }
    return;
  }

  session->progress.reset();

  /* peak memory usage should show current render peak, not peak for all renders
   * made by this render session
   */
  session->stats.mem_peak = session->stats.mem_used;

  if (is_new_session) {
    /* Sync object should be re-created for new scene. */
    sync = make_unique<BlenderSync>(
        b_engine, b_data, b_scene, scene, !background, use_developer_ui, session->progress);
  }
  else {
    /* Sync recalculations to do just the required updates. */
    sync->sync_recalc(b_depsgraph, b_v3d, b_rv3d);
  }

  BL::Object b_camera_override(b_engine.camera_override());
  sync->sync_camera(b_render, b_camera_override, width, height, "");

  BL::SpaceView3D b_null_space_view3d(PointerRNA_NULL);
  BL::RegionView3D b_null_region_view3d(PointerRNA_NULL);
  const BufferParams buffer_params = BlenderSync::get_buffer_params(
      b_null_space_view3d, b_null_region_view3d, scene->camera, width, height);
  session->reset(session_params, buffer_params);

  /* reset time */
  start_resize_time = 0.0;

  {
    const thread_scoped_lock lock(draw_state_.mutex);
    draw_state_.last_pass_index = -1;
  }
}

void BlenderSession::free_session()
{
  if (session) {
    session->cancel(true);
  }

  sync.reset();
  session.reset();

  display_driver_ = nullptr;
}

void BlenderSession::full_buffer_written(string_view filename)
{
  full_buffer_files_.emplace_back(filename);
}

static void add_cryptomatte_layer(BL::RenderResult &b_rr, string name, string manifest)
{
  const string identifier = string_printf("%08x",
                                          util_murmur_hash3(name.c_str(), name.length(), 0));
  const string prefix = "cryptomatte/" + identifier.substr(0, 7) + "/";

  render_add_metadata(b_rr, prefix + "name", name);
  render_add_metadata(b_rr, prefix + "hash", "MurmurHash3_32");
  render_add_metadata(b_rr, prefix + "conversion", "uint32_to_float32");
  render_add_metadata(b_rr, prefix + "manifest", manifest);
}

void BlenderSession::stamp_view_layer_metadata(Scene *scene, const string &view_layer_name)
{
  BL::RenderResult b_rr = b_engine.get_result();
  const string prefix = "cycles." + view_layer_name + ".";

  /* Configured number of samples for the view layer. */
  b_rr.stamp_data_add_field((prefix + "samples").c_str(),
                            to_string(session->params.samples).c_str());

  /* Store ranged samples information. */
  /* TODO(sergey): Need to bring this information back. */
#if 0
  if (session->tile_manager.range_num_samples != -1) {
    b_rr.stamp_data_add_field((prefix + "range_start_sample").c_str(),
                              to_string(session->tile_manager.range_start_sample).c_str());
    b_rr.stamp_data_add_field((prefix + "range_num_samples").c_str(),
                              to_string(session->tile_manager.range_num_samples).c_str());
  }
#endif

  /* Write cryptomatte metadata. */
  if (scene->film->get_cryptomatte_passes() & CRYPT_OBJECT) {
    add_cryptomatte_layer(b_rr,
                          view_layer_name + ".CryptoObject",
                          scene->object_manager->get_cryptomatte_objects(scene));
  }
  if (scene->film->get_cryptomatte_passes() & CRYPT_MATERIAL) {
    add_cryptomatte_layer(b_rr,
                          view_layer_name + ".CryptoMaterial",
                          scene->shader_manager->get_cryptomatte_materials(scene));
  }
  if (scene->film->get_cryptomatte_passes() & CRYPT_ASSET) {
    add_cryptomatte_layer(b_rr,
                          view_layer_name + ".CryptoAsset",
                          scene->object_manager->get_cryptomatte_assets(scene));
  }

  /* Store synchronization and bare-render times. */
  double total_time;
  double render_time;
  session->progress.get_time(total_time, render_time);
  b_rr.stamp_data_add_field((prefix + "total_time").c_str(),
                            time_human_readable_from_seconds(total_time).c_str());
  b_rr.stamp_data_add_field((prefix + "render_time").c_str(),
                            time_human_readable_from_seconds(render_time).c_str());
  b_rr.stamp_data_add_field((prefix + "synchronization_time").c_str(),
                            time_human_readable_from_seconds(total_time - render_time).c_str());
}

void BlenderSession::render(BL::Depsgraph &b_depsgraph_)
{
  b_depsgraph = b_depsgraph_;

  if (session->progress.get_cancel()) {
    update_status_progress();
    return;
  }

  /* Create driver to write out render results. */
  if (!braas_hpc_options) {
    ensure_display_driver_if_needed();
    session->set_output_driver(make_unique<BlenderOutputDriver>(b_engine));

    session->full_buffer_written_cb = [&](string_view filename) { full_buffer_written(filename); };
  }

  BL::ViewLayer b_view_layer = b_depsgraph.view_layer_eval();

  /* get buffer parameters */
  const SessionParams session_params = get_session_params(
      b_engine, b_userpref, b_scene, background);
  BufferParams buffer_params = BlenderSync::get_buffer_params(
      b_v3d, b_rv3d, scene->camera, width, height);

  /* temporary render result to find needed passes and views */
  BL::RenderResult b_rr = b_engine.begin_result(0, 0, 1, 1, b_view_layer.name().c_str(), nullptr);
  BL::RenderResult::layers_iterator b_single_rlay;
  b_rr.layers.begin(b_single_rlay);
  BL::RenderLayer b_rlay = *b_single_rlay;

  {
    const thread_scoped_lock lock(draw_state_.mutex);
    b_rlay_name = b_view_layer.name();

    /* Signal that the display pass is to be updated. */
    draw_state_.last_pass_index = -1;
  }

  /* Compute render passes and film settings. */
  sync->sync_render_passes(b_rlay, b_view_layer);

  BL::RenderResult::views_iterator b_view_iter;

  int num_views = 0;
  for (b_rr.views.begin(b_view_iter); b_view_iter != b_rr.views.end(); ++b_view_iter) {
    num_views++;
  }

  int view_index = 0;
  for (b_rr.views.begin(b_view_iter); b_view_iter != b_rr.views.end(); ++b_view_iter, ++view_index)
  {
    b_rview_name = b_view_iter->name();

    buffer_params.layer = b_view_layer.name();
    buffer_params.view = b_rview_name;

    /* set the current view */
    b_engine.active_view_set(b_rview_name.c_str());

    /* Force update in this case, since the camera transform on each frame changes
     * in different views. This could be optimized by somehow storing the animated
     * camera transforms separate from the fixed stereo transform. */
    if ((scene->need_motion() != Scene::MOTION_NONE) && view_index > 0) {
      sync->tag_update();
    }

    /* update scene */
    BL::Object b_camera_override(b_engine.camera_override());
    sync->sync_camera(b_render, b_camera_override, width, height, b_rview_name.c_str());
    sync->sync_data(b_render,
                    b_depsgraph,
                    b_v3d,
                    b_camera_override,
                    width,
                    height,
                    &python_thread_state,
                    session_params.denoise_device);

    /* At the moment we only free if we are not doing multi-view
     * (or if we are rendering the last view). See #58142/D4239 for discussion.
     */
    const bool can_free_cache = (view_index == num_views - 1);
    if (can_free_cache) {
      sync->free_data_after_sync(b_depsgraph);
    }

    builtin_images_load();

    /* Attempt to free all data which is held by Blender side, since at this
     * point we know that we've got everything to render current view layer.
     */
    if (can_free_cache) {
      free_blender_memory_if_possible();
    }

    /* Make sure all views have different noise patterns. - hardcoded value just to make it random
     */
    if (view_index != 0) {
      int seed = scene->integrator->get_seed();
      seed += hash_uint2(seed, hash_uint2(view_index * 0xdeadbeef, 0));
      scene->integrator->set_seed(seed);
    }

    /* Update number of samples per layer. */
    const int samples = sync->get_layer_samples();
    const bool bound_samples = sync->get_layer_bound_samples();

    SessionParams effective_session_params = session_params;
    if (samples != 0 && (!bound_samples || (samples < session_params.samples))) {
      effective_session_params.samples = samples;
    }

    /* Update session itself. */
    session->reset(effective_session_params, buffer_params);

    /* render */
    if (!b_engine.is_preview() && background && print_render_stats) {
      scene->enable_update_stats();
    }

    if (braas_hpc_options) {
      braas_hpc_run();
    } else {
      session->start();
      session->wait();
    }

    if (!b_engine.is_preview() && background && print_render_stats) {
      RenderStats stats;
      session->collect_statistics(&stats);
      printf("Render statistics:\n%s\n", stats.full_report().c_str());
    }

    if (session->progress.get_cancel()) {
      break;
    }
  }

  /* add metadata */
  stamp_view_layer_metadata(scene, b_rlay_name);

  /* free result without merging */
  b_engine.end_result(b_rr, true, false, false);

  /* When tiled rendering is used there will be no "write" done for the tile. Forcefully clear
   * highlighted tiles now, so that the highlight will be removed while processing full frame from
   * file. */
  b_engine.tile_highlight_clear_all();

  double total_time;
  double render_time;
  session->progress.get_time(total_time, render_time);
  VLOG_INFO << "Total render time: " << total_time;
  VLOG_INFO << "Render time (without synchronization): " << render_time;
}

void BlenderSession::render_frame_finish()
{
  /* Processing of all layers and views is done. Clear the strings so that we can communicate
   * progress about reading files and denoising them. */
  b_rlay_name = "";
  b_rview_name = "";

  if (!b_render.use_persistent_data()) {
    /* Free the sync object so that it can properly dereference nodes from the scene graph before
     * the graph is freed. */
    sync.reset();

    session->device_free();
  }

  for (const string_view filename : full_buffer_files_) {
    session->process_full_buffer_from_disk(filename);
    if (check_and_report_session_error()) {
      break;
    }
  }

  for (const string_view filename : full_buffer_files_) {
    path_remove(filename);
  }

  /* Clear output driver. */
  session->set_output_driver(nullptr);
  session->full_buffer_written_cb = nullptr;

  /* The display driver is the source of drawing context for both drawing and possible graphics
   * interoperability objects in the path trace. Once the frame is finished the OpenGL context
   * might be freed form Blender side. Need to ensure that all GPU resources are freed prior to
   * that point.
   * Ideally would only do this when OpenGL context is actually destroyed, but there is no way to
   * know when this happens (at least in the code at the time when this comment was written).
   * The penalty of re-creating resources on every frame is unlikely to be noticed. */
  display_driver_ = nullptr;
  session->set_display_driver(nullptr);

  /* All the files are handled.
   * Clear the list so that this session can be re-used by Persistent Data. */
  full_buffer_files_.clear();
}

static bool bake_setup_pass(Scene *scene, const string &bake_type, const int bake_filter)
{
  Integrator *integrator = scene->integrator;
  Film *film = scene->film;

  const bool filter_direct = (bake_filter & BL::BakeSettings::pass_filter_DIRECT) != 0;
  const bool filter_indirect = (bake_filter & BL::BakeSettings::pass_filter_INDIRECT) != 0;
  const bool filter_color = (bake_filter & BL::BakeSettings::pass_filter_COLOR) != 0;

  PassType type = PASS_NONE;
  bool use_direct_light = false;
  bool use_indirect_light = false;
  bool include_albedo = false;

  /* Data passes. */
  if (bake_type == "POSITION") {
    type = PASS_POSITION;
  }
  else if (bake_type == "NORMAL") {
    type = PASS_NORMAL;
  }
  else if (bake_type == "UV") {
    type = PASS_UV;
  }
  else if (bake_type == "ROUGHNESS") {
    type = PASS_ROUGHNESS;
  }
  else if (bake_type == "EMIT") {
    type = PASS_EMISSION;
  }
  /* Environment pass. */
  else if (bake_type == "ENVIRONMENT") {
    type = PASS_BACKGROUND;
  }
  /* AO pass. */
  else if (bake_type == "AO") {
    type = PASS_AO;
  }
  /* Shadow pass. */
  else if (bake_type == "SHADOW") {
    /* Bake as combined pass, together with marking the object as a shadow catcher. */
    type = PASS_SHADOW_CATCHER;
    film->set_use_approximate_shadow_catcher(true);

    use_direct_light = true;
    use_indirect_light = true;
    include_albedo = true;

    integrator->set_use_diffuse(true);
    integrator->set_use_glossy(true);
    integrator->set_use_transmission(true);
    integrator->set_use_emission(true);
  }
  /* Combined pass. */
  else if (bake_type == "COMBINED") {
    type = PASS_COMBINED;
    film->set_use_approximate_shadow_catcher(true);

    use_direct_light = filter_direct;
    use_indirect_light = filter_indirect;
    include_albedo = filter_color;

    integrator->set_use_diffuse((bake_filter & BL::BakeSettings::pass_filter_DIFFUSE) != 0);
    integrator->set_use_glossy((bake_filter & BL::BakeSettings::pass_filter_GLOSSY) != 0);
    integrator->set_use_transmission((bake_filter & BL::BakeSettings::pass_filter_TRANSMISSION) !=
                                     0);
    integrator->set_use_emission((bake_filter & BL::BakeSettings::pass_filter_EMIT) != 0);
  }
  /* Light component passes. */
  else if ((bake_type == "DIFFUSE") || (bake_type == "GLOSSY") || (bake_type == "TRANSMISSION")) {
    use_direct_light = filter_direct;
    use_indirect_light = filter_indirect;
    include_albedo = filter_color;

    integrator->set_use_diffuse(bake_type == "DIFFUSE");
    integrator->set_use_glossy(bake_type == "GLOSSY");
    integrator->set_use_transmission(bake_type == "TRANSMISSION");

    if (bake_type == "DIFFUSE") {
      if (filter_direct && filter_indirect) {
        type = PASS_DIFFUSE;
      }
      else if (filter_direct) {
        type = PASS_DIFFUSE_DIRECT;
      }
      else if (filter_indirect) {
        type = PASS_DIFFUSE_INDIRECT;
      }
      else {
        type = PASS_DIFFUSE_COLOR;
      }
    }
    else if (bake_type == "GLOSSY") {
      if (filter_direct && filter_indirect) {
        type = PASS_GLOSSY;
      }
      else if (filter_direct) {
        type = PASS_GLOSSY_DIRECT;
      }
      else if (filter_indirect) {
        type = PASS_GLOSSY_INDIRECT;
      }
      else {
        type = PASS_GLOSSY_COLOR;
      }
    }
    else if (bake_type == "TRANSMISSION") {
      if (filter_direct && filter_indirect) {
        type = PASS_TRANSMISSION;
      }
      else if (filter_direct) {
        type = PASS_TRANSMISSION_DIRECT;
      }
      else if (filter_indirect) {
        type = PASS_TRANSMISSION_INDIRECT;
      }
      else {
        type = PASS_TRANSMISSION_COLOR;
      }
    }
  }

  if (type == PASS_NONE) {
    return false;
  }

  /* Create pass. */
  Pass *pass = scene->create_node<Pass>();
  pass->set_name(ustring("Combined"));
  pass->set_type(type);
  pass->set_include_albedo(include_albedo);

  /* Disable direct indirect light for performance when not needed. */
  integrator->set_use_direct_light(use_direct_light);
  integrator->set_use_indirect_light(use_indirect_light);

  /* Disable denoiser if the pass does not support it.
   * For the passes which support denoising follow the user configuration. */
  const PassInfo pass_info = Pass::get_info(type);
  if (integrator->get_use_denoise() && !pass_info.support_denoise) {
    integrator->set_use_denoise(false);
  }

  return true;
}

void BlenderSession::bake(BL::Depsgraph &b_depsgraph_,
                          BL::Object &b_object,
                          const string &bake_type,
                          const int bake_filter,
                          const int bake_width,
                          const int bake_height)
{
  b_depsgraph = b_depsgraph_;

  /* Get session parameters. */
  const SessionParams session_params = get_session_params(
      b_engine, b_userpref, b_scene, background);

  /* Initialize bake manager, before we load the baking kernels. */
  scene->bake_manager->set_baking(scene, true);

  session->set_display_driver(nullptr);
  session->set_output_driver(make_unique<BlenderOutputDriver>(b_engine));
  session->full_buffer_written_cb = [&](string_view filename) { full_buffer_written(filename); };

  /* Sync scene. */
  BL::Object b_camera_override(b_engine.camera_override());
  sync->set_bake_target(b_object);
  sync->sync_camera(b_render, b_camera_override, width, height, "");
  sync->sync_data(b_render,
                  b_depsgraph,
                  b_v3d,
                  b_camera_override,
                  width,
                  height,
                  &python_thread_state,
                  session_params.denoise_device);

  /* Save the current state of the denoiser, as it might be disabled by the pass configuration (for
   * passed which do not support denoising). */
  Integrator *integrator = scene->integrator;
  const bool was_denoiser_enabled = integrator->get_use_denoise();

  /* Add render pass that we want to bake, and name it Combined so that it is
   * used as that on the Blender side. */
  if (!bake_setup_pass(scene, bake_type, bake_filter)) {
    session->cancel(true);
  }

  /* Always use transparent background for baking. */
  scene->background->set_transparent(true);

  if (!session->progress.get_cancel()) {
    /* Load built-in images from Blender. */
    builtin_images_load();
  }

  /* Object might have been disabled for rendering or excluded in some
   * other way, in that case Blender will report a warning afterwards. */
  Object *bake_object = nullptr;
  if (!session->progress.get_cancel()) {
    for (Object *ob : scene->objects) {
      if (ob->get_is_bake_target()) {
        bake_object = ob;
        break;
      }
    }
  }

  /* For the shadow pass, temporarily mark the object as a shadow catcher. */
  const bool was_shadow_catcher = (bake_object) ? bake_object->get_is_shadow_catcher() : false;
  if (bake_object && bake_type == "SHADOW") {
    bake_object->set_is_shadow_catcher(true);
  }

  if (bake_object && !session->progress.get_cancel()) {
    /* Get buffer parameters. */
    BufferParams buffer_params;
    buffer_params.width = bake_width;
    buffer_params.height = bake_height;
    buffer_params.window_width = bake_width;
    buffer_params.window_height = bake_height;
    /* Unique layer name for multi-image baking. */
    buffer_params.layer = string_printf("bake_%d\n", bake_id++);

    /* Update session. */
    session->reset(session_params, buffer_params);

    session->progress.set_update_callback([this] { update_bake_progress(); });
  }

  /* Perform bake. Check cancel to avoid crash with incomplete scene data. */
  if (bake_object && !session->progress.get_cancel()) {
    session->start();
    session->wait();
  }

  /* Restore object state. */
  if (bake_object) {
    bake_object->set_is_shadow_catcher(was_shadow_catcher);
  }

  /* Restore the state of denoiser to before it was possibly disabled by the pass, so that the
   * next baking pass can use the original value. */
  integrator->set_use_denoise(was_denoiser_enabled);
}

void BlenderSession::synchronize(BL::Depsgraph &b_depsgraph_)
{
  /* only used for viewport render */
  if (!b_v3d) {
    return;
  }

  /* on session/scene parameter changes, we recreate session entirely */
  const SessionParams session_params = get_session_params(
      b_engine, b_userpref, b_scene, background);
  const SceneParams scene_params = get_scene_params(
      b_scene, background, use_developer_ui);
  const bool session_pause = BlenderSync::get_session_pause(b_scene, background);

  if (session->params.modified(session_params) || scene->params.modified(scene_params)) {
    free_session();
    create_session();
  }

  ensure_display_driver_if_needed();

  /* increase samples and render time, but never decrease */
  session->set_samples(session_params.samples);
  session->set_time_limit(session_params.time_limit);
  session->set_pause(session_pause);

  /* copy recalc flags, outside of mutex so we can decide to do the real
   * synchronization at a later time to not block on running updates */
  sync->sync_recalc(b_depsgraph_, b_v3d, b_rv3d);

  /* don't do synchronization if on pause */
  if (session_pause) {
    tag_update();
    return;
  }

  /* try to acquire mutex. if we don't want to or can't, come back later */
  if (!session->ready_to_reset() || !session->scene->mutex.try_lock()) {
    tag_update();
    return;
  }

  /* data and camera synchronize */
  b_depsgraph = b_depsgraph_;

  BL::Object b_camera_override(b_engine.camera_override());
  sync->sync_data(b_render,
                  b_depsgraph,
                  b_v3d,
                  b_camera_override,
                  width,
                  height,
                  &python_thread_state,
                  session_params.denoise_device);

  if (b_rv3d) {
    sync->sync_view(b_v3d, b_rv3d, width, height);
  }
  else {
    sync->sync_camera(b_render, b_camera_override, width, height, "");
  }

  /* get buffer parameters */
  const BufferParams buffer_params = BlenderSync::get_buffer_params(
      b_v3d, b_rv3d, scene->camera, width, height);

  /* reset if needed */
  if (scene->need_reset()) {
    session->reset(session_params, buffer_params);

    /* After session reset, so device is not accessing image data anymore. */
    builtin_images_load();

    /* reset time */
    start_resize_time = 0.0;
  }

  /* unlock */
  session->scene->mutex.unlock();

  /* Start rendering thread, if it's not running already. Do this
   * after all scene data has been synced at least once. */
  session->start();
}

void BlenderSession::draw(BL::SpaceImageEditor &space_image)
{
  if (!session || !session->scene) {
    /* Offline render drawing does not force the render engine update, which means it's possible
     * that the Session is not created yet. */
    return;
  }

  const thread_scoped_lock lock(draw_state_.mutex);

  const int pass_index = space_image.image_user().multilayer_pass();
  if (pass_index != draw_state_.last_pass_index) {
    BL::RenderPass b_display_pass(b_engine.pass_by_index_get(b_rlay_name.c_str(), pass_index));
    if (!b_display_pass) {
      return;
    }

    Scene *scene = session->scene.get();

    const thread_scoped_lock lock(scene->mutex);

    const Pass *pass = Pass::find(scene->passes, b_display_pass.name());
    if (!pass) {
      return;
    }

    scene->film->set_display_pass(pass->get_type());

    draw_state_.last_pass_index = pass_index;
  }

  if (display_driver_) {
    BL::Array<float, 2> zoom = space_image.zoom();
    display_driver_->set_zoom(zoom[0], zoom[1]);
  }

  session->draw();
}

void BlenderSession::view_draw(const int w, const int h)
{
  /* pause in redraw in case update is not being called due to final render */
  session->set_pause(BlenderSync::get_session_pause(b_scene, background));

  /* before drawing, we verify camera and viewport size changes, because
   * we do not get update callbacks for those, we must detect them here */
  if (session->ready_to_reset()) {
    bool reset = false;

    /* if dimensions changed, reset */
    if (width != w || height != h) {
      if (start_resize_time == 0.0) {
        /* don't react immediately to resizes to avoid flickery resizing
         * of the viewport, and some window managers changing the window
         * size temporarily on unminimize */
        start_resize_time = time_dt();
        tag_redraw();
      }
      else if (time_dt() - start_resize_time < 0.2) {
        tag_redraw();
      }
      else {
        width = w;
        height = h;
        reset = true;
      }
    }

    /* try to acquire mutex. if we can't, come back later */
    if (!session->scene->mutex.try_lock()) {
      tag_update();
    }
    else {
      /* update camera from 3d view */

      sync->sync_view(b_v3d, b_rv3d, width, height);

      if (scene->camera->is_modified()) {
        reset = true;
      }

      session->scene->mutex.unlock();
    }

    /* reset if requested */
    if (reset) {
      const SessionParams session_params = get_session_params(
          b_engine, b_userpref, b_scene, background);
      const BufferParams buffer_params = BlenderSync::get_buffer_params(
          b_v3d, b_rv3d, scene->camera, width, height);
      const bool session_pause = BlenderSync::get_session_pause(b_scene, background);

      if (session_pause == false) {
        session->reset(session_params, buffer_params);
        start_resize_time = 0.0;
      }
    }
  }
  else {
    tag_update();
  }

  /* update status and progress for 3d view draw */
  update_status_progress();

  /* draw */
  session->draw();
}

void BlenderSession::get_status(string &status, string &substatus)
{
  session->progress.get_status(status, substatus);
}

void BlenderSession::get_progress(double &progress, double &total_time, double &render_time)
{
  session->progress.get_time(total_time, render_time);
  progress = session->progress.get_progress();
}

void BlenderSession::update_bake_progress()
{
  const double progress = session->progress.get_progress();

  if (progress != last_progress) {
    b_engine.update_progress((float)progress);
    last_progress = progress;
  }
}

void BlenderSession::update_status_progress()
{
  string timestatus;
  string status;
  string substatus;
  string scene_status;
  double progress;
  double total_time;
  double remaining_time = 0;
  double render_time;
  const float mem_used = (float)session->stats.mem_used / 1024.0f / 1024.0f;
  const float mem_peak = (float)session->stats.mem_peak / 1024.0f / 1024.0f;

  get_status(status, substatus);
  get_progress(progress, total_time, render_time);

  if (progress > 0) {
    remaining_time = session->get_estimated_remaining_time();
  }

  if (background) {
    if (scene) {
      scene_status += " | " + scene->name;
    }
    if (!b_rlay_name.empty()) {
      scene_status += ", " + b_rlay_name;
    }

    if (!b_rview_name.empty()) {
      scene_status += ", " + b_rview_name;
    }

    if (remaining_time > 0) {
      timestatus += "Remaining:" + time_human_readable_from_seconds(remaining_time) + " | ";
    }

    timestatus += string_printf("Mem:%.2fM, Peak:%.2fM", (double)mem_used, (double)mem_peak);

    if (!status.empty()) {
      status = " | " + status;
    }
    if (!substatus.empty()) {
      status += " | " + substatus;
    }
  }

  const double current_time = time_dt();
  /* When rendering in a window, redraw the status at least once per second to keep the elapsed
   * and remaining time up-to-date. For headless rendering, only report when something
   * significant changes to keep the console output readable. */
  if (status != last_status || (!headless && (current_time - last_status_time) > 1.0)) {
    b_engine.update_stats("", (timestatus + scene_status + status).c_str());
    b_engine.update_memory_stats(mem_used, mem_peak);
    last_status = status;
    last_status_time = current_time;
  }
  if (progress != last_progress) {
    b_engine.update_progress((float)progress);
    last_progress = progress;
  }

  check_and_report_session_error();
}

bool BlenderSession::check_and_report_session_error()
{
  if (!session->progress.get_error()) {
    return false;
  }

  const string error = session->progress.get_error_message();
  if (error != last_error) {
    /* TODO(sergey): Currently C++ RNA API doesn't let us to use mnemonic name for the variable.
     * Would be nice to have this figured out.
     *
     * For until then, 1 << 5 means RPT_ERROR. */
    b_engine.report(1 << 5, error.c_str());
    b_engine.error_set(error.c_str());
    last_error = error;
  }

  return true;
}

void BlenderSession::tag_update()
{
  /* tell blender that we want to get another update callback */
  b_engine.tag_update();
}

void BlenderSession::tag_redraw()
{
  if (background) {
    /* update stats and progress, only for background here because
     * in 3d view we do it in draw for thread safety reasons */
    update_status_progress();

    /* offline render, redraw if timeout passed */
    if (time_dt() - last_redraw_time > 1.0) {
      b_engine.tag_redraw();
      last_redraw_time = time_dt();
    }
  }
  else {
    /* tell blender that we want to redraw */
    b_engine.tag_redraw();
  }
}

void BlenderSession::test_cancel()
{
  /* test if we need to cancel rendering */
  if (background) {
    if (b_engine.test_break()) {
      session->progress.set_cancel("Cancelled");
    }
  }
}

void BlenderSession::free_blender_memory_if_possible()
{
  if (!background) {
    /* During interactive render we can not free anything: attempts to save
     * memory would cause things to be allocated and evaluated for every
     * updated sample.
     */
    return;
  }
  b_engine.free_blender_memory();
}

void BlenderSession::ensure_display_driver_if_needed()
{
  if (display_driver_) {
    /* Driver is already created. */
    return;
  }

  if (headless) {
    /* No display needed for headless. */
    return;
  }

  if (b_engine.is_preview()) {
    /* TODO(sergey): Investigate whether DisplayDriver can be used for the preview as well. */
    return;
  }

  unique_ptr<BlenderDisplayDriver> display_driver = make_unique<BlenderDisplayDriver>(
      b_engine, b_scene, background);
  display_driver_ = display_driver.get();
  session->set_display_driver(std::move(display_driver));
}

CCL_NAMESPACE_END
