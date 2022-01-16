
u32            ItemCullDataCount = 0;
item_cull_data ItemCullData[1024];

ItemCullData[ItemCullDataCount++] = {.CP={}, .ItemIndex=1, .P={1, 4, 3}, .Q={0,0,0,1}};
ItemCullData[ItemCullDataCount++] = {.CP={}, .ItemIndex=1, .P={1, 2, 3}, .Q={0,0,0,1}};
ItemCullData[ItemCullDataCount++] = {.CP={}, .ItemIndex=0, .P={1, 1, 3.5f}, .Q={0,0,0,1}};
ItemCullData[ItemCullDataCount++] = {.CP={}, .ItemIndex=1, .P={2, 1, 3}, .Q={0,0,0,1}};
ItemCullData[ItemCullDataCount++] = {.CP={}, .ItemIndex=1, .P={3, 1, 3}, .Q={0,0,0,1}};
ItemCullData[ItemCullDataCount++] = {.CP={}, .ItemIndex=1, .P={2.5f, 2, 3.5f}, .Q=LookRotation({1,0,0}, {0,1,0})};
ItemCullData[ItemCullDataCount++] = {.CP={}, .ItemIndex=2, .P={3.5f, 2, 3.5f}, .Q=Normalize(LookRotation({1,1,1}, {1,0,1}))};

static void
CullItem(game_state *State, camera_cull_push *CompCull)
{
    if(ItemCullDataCount == 0) return;
    { MarkScoped("Item: Cull", v4{1,1,0,1});
        CompCull->MaxDrawCommandCount = ItemCullDataCount;
        VkBufferMemoryBarrier PrefillBarriers[2] =
        {
            BufferBarrier(ItemDrawCountBuffer.Buffer, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT),
            BufferBarrier(ItemCullDataBuffer.Buffer,  VK_ACCESS_MEMORY_READ_BIT,           VK_ACCESS_TRANSFER_WRITE_BIT),
        };
        vkCmdPipelineBarrier(CommandBuffer, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, ArrayCount(PrefillBarriers), PrefillBarriers, 0, nullptr);
        vkCmdFillBuffer(CommandBuffer, ItemDrawCountBuffer.Buffer, 0, 4, 0);
        {
            u64 Size = ItemCullDataCount*sizeof(*ItemCullData);
            VkBufferCopy C =
            {
                .srcOffset = 0,
                .dstOffset = 0,
                .size      = Size,
            };
            memcpy((u8 *)TransferBuffer.Data, ItemCullData, Size);
            vkCmdCopyBuffer(CommandBuffer, TransferBuffer.Buffer, ItemCullDataBuffer.Buffer, 1, &C);
        }
        VkBufferMemoryBarrier FillBarriers[2] =
        {
            BufferBarrier(ItemDrawCountBuffer.Buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
            BufferBarrier(ItemCullDataBuffer.Buffer,  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
        };
        vkCmdPipelineBarrier(CommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, ArrayCount(FillBarriers), FillBarriers, 0, nullptr);
        vkCmdBindPipeline(CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ItemCullPipeline);
        descriptor_info Descriptors[] =
        {
            ItemCullDataBuffer.Buffer,
            ItemDrawCommandBuffer.Buffer,
            ItemDrawCountBuffer.Buffer,
            ItemDrawDataBuffer.Buffer,
            ItemDrawInfoBuffer.Buffer,
        };
        PushDescriptors   (CommandBuffer, ItemCullProgram, Descriptors);
        vkCmdPushConstants(CommandBuffer, ItemCullProgram.Layout, ItemCullProgram.PushConstantStages, 0, sizeof(*CompCull), CompCull);
        vkCmdDispatch     (CommandBuffer, (u32)AlignUp((s32)ItemCullDataCount, ITEM_CULL_X_COUNT), 1, 1);
        VkBufferMemoryBarrier CullBarriers[] =
        {
            BufferBarrier(ItemDrawCommandBuffer.Buffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT),
            BufferBarrier(ItemDrawCountBuffer.Buffer,   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT),
        };
        vkCmdPipelineBarrier(CommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 0, nullptr, ArrayCount(CullBarriers), CullBarriers, 0, nullptr);
    }
}

static void
RenderItem(world_draw_push *Push)
{
    MarkScoped("Item");
    vkCmdBindPipeline(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ItemDrawPipeline);
    descriptor_info Descriptors[] =
    {
        ItemDrawDataBuffer.Buffer, ItemVertexBuffer.Buffer,
        descriptor_info(Sampler, Atlas.View, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
    };
    PushDescriptors     (CommandBuffer, ItemDrawProgram, Descriptors);
    vkCmdBindIndexBuffer(CommandBuffer, ItemIndexBuffer.Buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants  (CommandBuffer, ItemDrawProgram.Layout, ItemDrawProgram.PushConstantStages, 0, sizeof(*Push), Push);
    u32 MaxDrawCount = ItemCullDataCount;
    vkCmdDrawIndexedIndirectCount(CommandBuffer, ItemDrawCommandBuffer.Buffer, 0, ItemDrawCountBuffer.Buffer, 0, MaxDrawCount, sizeof(VkDrawIndexedIndirectCommand));
}

// item cull shader

#version 460
#include "item.h"

layout(local_size_x_id=0) in;

layout(push_constant, std140) uniform block { camera_cull_push _; };
layout(binding=0) readonly  buffer InD      { item_cull_data Data[]; };
layout(binding=1) writeonly buffer OutC     { draw_indexed_indirect_command Commands[]; };
layout(binding=2)           buffer OutCount { u32 DrawCommandCount; };
layout(binding=3) writeonly buffer OutD     { item_draw_data Out[]; };
layout(binding=4) readonly  buffer InI      { item_draw_info Item[]; };

void
main()
{
    u32 di = gl_GlobalInvocationID.x;
    if(di >= _.MaxDrawCommandCount) return;
    item_cull_data I = Data[gl_GlobalInvocationID.x];
    {
        v3s RelC = I.CP - _.CP;
        v3  RelP = v3(RelC*C_SIZE) + (I.P - _.P);
        if(IsVisible(RelP))
        {
            item_draw_info D = Item[I.ItemIndex];
            u32 i = atomicAdd(DrawCommandCount, 1);
            Commands[i].IndexCount    = D.IndexCount;
            Commands[i].InstanceCount = 1;
            Commands[i].IndexFirst    = D.IndexFirst;
            Commands[i].VertexOffset  = 0;
            Commands[i].InstanceFirst = 0;
            Out[i].P = RelP;
            Out[i].TextureIndex = D.TextureIndex;
            Out[i].Q = I.Q;
        }
    }
}

// item vertex shader

#version 460 core
#include "item.h"

layout(location=0) flat out frag_texture OutT;
layout(location=1)      out frag         Out;

layout(push_constant, std140) uniform block { world_draw_push _; };
layout(binding=0) readonly buffer In { item_draw_data DrawData[]; };
layout(binding=1) readonly buffer V  { item_vertex    Vertices[]; };

void
main()
{
    item_draw_data Draw = DrawData[gl_DrawID];
    item_vertex    D    = Vertices[gl_VertexIndex];
    v3 Displacement = Draw.P + Rotate(D.P, Draw.Q);
    v3 TransformedP = Rotate(Displacement, _.Rotation);
    gl_Position = Perspective(TransformedP, _.PerspectiveParameters);
    Out.TextureP_LightIntensity = v3(D.T, 1.0f);
    Out.P   = TransformedP;
    OutT.ID = Draw.TextureIndex;
}

// item fragment shader

#version 460
#include "item.h"

layout(location=0) out v4 Color;
layout(location=0) flat in frag_texture InT;
layout(location=1)      in frag         In;

layout(push_constant, std140) uniform block { world_draw_push _; };
layout(binding=2) uniform sampler2DArray TextureSampler;

void
main()
{
    v2  TextureP       = In.TextureP_LightIntensity.xy;
    f32 LightIntensity = In.TextureP_LightIntensity.z;
    u32 ID             = InT.ID;
    v4 C               = texture(TextureSampler, v3(TextureP, ID));
    if(C.w == 0) discard;
    Color = v4(Fog(pow(LightIntensity, C.w)*C.xyz, length(In.P), _.FogParameters, _.FogColor), 1);
}
