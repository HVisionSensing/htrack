#include "cudax/kernel.h"
#include "cudax/functors/ImageGradient.h"
#include "cudax/Kinematic.h"

//== NAMESPACE ================================================================
namespace cudax {
//=============================================================================

/// Computes the content of a row of the jacobian
struct ComputeJacobianRowSilho : public thrust::unary_function<void, MeshGrid::Elem>{
    ImageGradient gradient;
    CustomJointInfo* jointinfos; ///< device
    ChainElement* chains; ///< device    
    int2* cnstr_indexes; ///< raw
    uchar* silhouette_sensor; ///< raw
    J_row* J_raw; ///< array of rows 
    float* e_raw; ///< right hand side (constraints)
    float weight;
    
    ComputeJacobianRowSilho(J_row* J_raw, float* e_raw) :
        J_raw(J_raw), e_raw(e_raw)
    {
        this->jointinfos = kinematic->jointinfos;
        this->chains = kinematic->chains;
        this->cnstr_indexes = pixel_indexer->cnstr_indexes;
        this->silhouette_sensor = thrust::raw_pointer_cast(cudax::silhouette_sensor->data());
        
        weight = settings->silho_weight;
    }
    
    __device__
    glm::mat3x2 projection_jacobian(const glm::vec3& pos){
        // printf("focal length: %f", focal_length);
        glm::mat3x2 M(0); ///< remember column major!
        M[0][0] = focal_length_x / pos[2];
        M[1][1] = focal_length_y / pos[2];
        M[2][0] = -pos[0] * focal_length_x / ( pos[2]*pos[2] );
        M[2][1] = -pos[1] * focal_length_y / ( pos[2]*pos[2] );
        return M;
    }
    
    __device__ int type(const MeshGrid::Elem& off){ return cnstr_indexes[off.z].x; }
    __device__ int constraint_index(const MeshGrid::Elem& off){ return cnstr_indexes[off.z].y; }   
    
    __device__
    void operator()(MeshGrid::Elem& off){
        if( type(off) != PixelType::CONSTRAINT_SILHO) return;
        
        int row_index = constraint_index(off);
        J_row* J_sub = J_raw + row_index;
        float* e_sub = e_raw + row_index;
        int joint_id = (int) tex2D(color_tex, off.x, off.y);
        
        ///--- Joint axis
        glm::vec3 pos;
        pos[0] = tex2D(extra_tex, off.x, off.y).x;
        pos[1] = tex2D(extra_tex, off.x, off.y).y;
        pos[2] = tex2D(extra_tex, off.x, off.y).z;
        
        /// Gradient of the silhouette image
        glm::vec2 grad = gradient(off);
        /// Jacobian of the projection matrix            
        glm::mat3x2 J_proj = projection_jacobian(pos);
        
        glm::vec3 projector = grad * J_proj;
        
        int Ss = (silhouette_sensor[off.z]>125); 
        int Sr = (joint_id<255); ///< white is BG

#define SKIP_INSIDE_CONSTRAINTS
#ifdef SKIP_INSIDE_CONSTRAINTS
        if(Ss==true)
           return; 
#endif

        ///--- Compute RHS
        *e_sub = weight*(Ss-Sr);
        
        ///--- Compute LHS
        for(int i_column=0; i_column<CHAIN_MAX_LENGTH; i_column++){
            int jointinfo_id = chains[joint_id].data[i_column];
            if(jointinfo_id==-1) break;
            const CustomJointInfo& jinfo = jointinfos[jointinfo_id];
            glm::vec3& axis = jointinfos[jointinfo_id].axis;           
            
            switch(jinfo.type){
                case 1:
                {
                    glm::vec3 col = glm::vec3( jointinfos[jointinfo_id].mat * glm::vec4( axis, 1 ) );
                    J_sub->data[jinfo.index] = weight * glm::dot(projector, col);
                    break;
                }
                case 0: // ROT
                {
                    glm::vec3 t(jointinfos[jointinfo_id].mat[3][0],jointinfos[jointinfo_id].mat[3][1],jointinfos[jointinfo_id].mat[3][2]);
                    glm::vec3 a = glm::normalize(glm::vec3(jointinfos[jointinfo_id].mat * glm::vec4( axis, 1 )) - t);
                    glm::vec3 col = glm::cross(a, pos - t);
                    J_sub->data[jinfo.index] = weight * glm::dot(projector, col);
                    break;
                }
            }
        }
    }
};

//=============================================================================
} // namespace cudax
//=============================================================================
