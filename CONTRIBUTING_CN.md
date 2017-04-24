如果您想要对 [xkcptun](https://github.com/liudf0716/xkcptun)项目做贡献, 请参考如下步骤和规则:

1. fork本项目先:

    ![fork](http://oi58.tinypic.com/jj2trm.jpg)

2. Clone 您fork完的项目:

    ```
    git clone git@github.com:your_github_username/xkcptun.git
    ```

3. clone后请创建一个branch分支:

    ```
    git checkout -b "xkcptun-1-fix"
    ```
    名称请任意定

4. 在该分支下修改您要提交的代码（提交前请测试通过）

5. 完成修改后不要忘记在 [contributor](https://github.com/liudf0716/xkcptun/blob/master/contributors.md)中添加您的大名.

6. Commit and push 您的更改, 在github上创建一个pull request.

    git commit --signoff  
    git push -f
    
7. Awaiting review, if accepted, merged!



**IMPORTANT**

切记，不要忘记更新您fork的xkcptun项目

Please, don't forget to update your fork. While you made your changes, the content of the `master` branch can change because other pull requests were merged and it can create conflicts. This is why you have to rebase on `master` every time before pushing your changes and check that your branch doesn't have any conflicts with `master`.
