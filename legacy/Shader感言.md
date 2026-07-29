嗯……我直接上权威文档，khronos爸爸说的。爸爸来了当头棒喝。
```
glUniform modifies the value of a uniform variable or a uniform variable array. The location of the uniform variable to be modified is specified by location, which should be a value returned by glGetUniformLocation. glUniform operates on the program object that was made part of current state by calling glUseProgram.
```
这算需要shader还是不需要shader。这应该是算的。毕竟有代码生成的location。但是没抛弃glUseProgram
回顾我的封装，我一直在用DSA风格的glProgramUniform。非DSA风格的glUniform是要use program的。那么问题来了，这个glUseProgram应该放在哪里。我们之前的use program可是在draw上面。

说到底，至少对于sampler来说，可能ShaderRef扔不掉了。

难绷，我或许触及到OpenGL的核心限制了。太久了，我早就忘掉了DSA并没有解放draw的vao和shader binding。

哎，好歹UBO和SSBO似乎没和shader绑定的那么死，嗯，没毛病，不然也不可能能从一个shader换绑到另一个shader。

我或许可以通过shader和render target来对场景里的物体分类后再进入渲染队列，扩大shader的作用域，来解决shader状态切换和shader满天飞的问题，在ecs的帮助下应该好说，给每个shader分类加tag就行，顶层根据shader tag先glUseProgram一次,然后在program作用域内再走渲染队列，然后所有shader类型走一遍渲染队列。毕竟shaderref还最终确实是每个渲染对象都必不可少的组件。但是我懒了，就这样吧。
这问题留给vulkan思考吧。
虽然现在完全回到OpenGL，在线编译shader，可以通过彻底的bindless texture解决这个问题，但是很遗憾，损失大于收益，不值当。不能为了一点点bindless texture的收益，放弃当前工作流。